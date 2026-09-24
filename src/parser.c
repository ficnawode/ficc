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
    ParserConfig cfg;
    const char *cur_func; /* enclosing function name for the `__func__` identifier */
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
    u32 stars;      /* explicit `*` count */
    u32 array_dims; /* explicit `[..]` count (not array-typedef base layers) */
    /* Parameters captured when the outermost suffix folded the signature. */
    Vec *func_params;
    bool func_variadic;
    Vec *attrs; /* Vec<Attr*> — GNU attributes on this declarator */
} Declarator;

typedef struct Specs
{
    Type *type;
    StorageClass storage;
    u32 alignas;
    ASTNode *tag_def;
    bool is_inline; /* the `inline` function specifier was seen (C11 §6.7.4) */
    Vec *attrs;     /* Vec<Attr*> — GNU attributes before the type */
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

/* A pedantic-only diagnostic; silent by default (GNU attribute tolerance). */
static void parse_warning(Parser *p, Loc loc, const char *fmt, ...)
{
    if (!p->cfg.pedantic)
    {
        return;
    }
    fprintf(stderr, "%s:%u:%u: [parse] warning: ", loc.file, loc.line, loc.col);
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
           k == TOK_KW_FLOAT || k == TOK_KW_DOUBLE || k == TOK_KW_STRUCT || k == TOK_KW_UNION ||
           k == TOK_KW_ENUM;
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

static size_t skip_quals(Parser *p, size_t pos)
{
    while (pos < p->count &&
           (p->tokens[pos].kind == TOK_KW_CONST || p->tokens[pos].kind == TOK_KW_VOLATILE ||
            p->tokens[pos].kind == TOK_KW_RESTRICT))
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
    return peek_token(p)->kind == TOK_LPAREN && is_typename_start_at(p, skip_quals(p, p->pos + 1));
}

static Type *parse_type_specifier(Parser *p);
static Type *apply_quals(Type *t, u8 quals);
static Type *parse_abstract_declarator(Parser *p, Type *base);
static Type *parse_array_suffix(Parser *p, Type *type, u32 *ndim_out);
static Vec *parse_param_list(Parser *p, bool *out_variadic);
static Specs parse_decl_specifiers(Parser *p);
static ASTNode *parse_expression(Parser *p);
static ASTNode *parse_assign(Parser *p);
static ASTNode *parse_initializer(Parser *p);
static ASTNode *parse_postfix(Parser *p);
static ASTNode *parse_postfix_ops(Parser *p, ASTNode *node);
static ASTNode *parse_init_list(Parser *p);
static ASTNode *parse_generic_selection(Parser *p, Loc loc);
static Type *parse_paren_type_name(Parser *p);
static Type *parse_type_name(Parser *p);
static ASTNode *parse_compound_stmt(Parser *p);
static ASTNode *parse_static_assert(Parser *p);
static ASTNode *parse_var_decl(Parser *p, Specs s);
static ASTNode *parse_stmt(Parser *p);
static bool folded_const(Parser *p, ASTNode *node, i64 *out);
static bool resolve_constant_init(Parser *p, ASTVarDecl *vd, ASTNode *expr);
static bool parse_declarator_core(Parser *p, Type *base, Declarator *out, bool name_optional,
                                  bool inner_group);
static bool parse_declarator(Parser *p, Type *base, Declarator *out);

static bool is_attribute_name(Token *t)
{
    return t->kind == TOK_IDENT && (strcmp(t->payload.str, "__attribute__") == 0 ||
                                    strcmp(t->payload.str, "__attribute") == 0);
}

/* Classifies one attribute name; `align` is filled for `aligned(n)`. */
static AttrKind attribute_kind(const char *name, u64 *align, bool *has_align)
{
    *has_align = false;
    *align = 0;
    if (strcmp(name, "packed") == 0 || strcmp(name, "__packed__") == 0)
    {
        return ATTR_PACKED;
    }
    if (strcmp(name, "aligned") == 0 || strcmp(name, "__aligned__") == 0)
    {
        return ATTR_ALIGNED;
    }
    if (strcmp(name, "constructor") == 0 || strcmp(name, "__constructor__") == 0)
    {
        return ATTR_CONSTRUCTOR;
    }
    if (strcmp(name, "destructor") == 0 || strcmp(name, "__destructor__") == 0)
    {
        return ATTR_DESTRUCTOR;
    }
    return ATTR_UNKNOWN;
}

/* An attribute name is an identifier; GNU spelling aliases such as `__const__`
   and `__noreturn__` lex as keywords but still carry their spelling. */
static bool attribute_name_token(Token *t)
{
    switch (t->kind)
    {
        case TOK_IDENT:
        case TOK_KW_CONST:
        case TOK_KW_VOLATILE:
        case TOK_KW_RESTRICT:
        case TOK_KW_INLINE:
        case TOK_KW_NORETURN:
        case TOK_KW_SIGNED:
        case TOK_KW_REGISTER:
        case TOK_KW_AUTO:
            return true;
        default:
            return false;
    }
}

/* Consumes one `__attribute__ (( name[(args)] , ... ))` group, appending an
   `Attr` per name to `out`. Arguments other than `aligned(n)` are parsed and
   discarded. */
static bool parse_attribute_list(Parser *p, Vec *out)
{
    next_token(p); /* __attribute__ */
    if (peek_token(p)->kind != TOK_LPAREN)
    {
        parse_error(p, "expected '(' after __attribute__");
        return false;
    }
    next_token(p);
    if (peek_token(p)->kind != TOK_LPAREN)
    {
        parse_error(p, "expected '((' after __attribute__");
        return false;
    }
    next_token(p);

    while (peek_token(p)->kind != TOK_RPAREN && peek_token(p)->kind != TOK_EOF)
    {
        Token *name = peek_token(p);
        if (!attribute_name_token(name))
        {
            parse_error(p, "expected attribute name");
            return false;
        }
        next_token(p);
        u64 align = 0;
        bool has_align = false;
        AttrKind kind = attribute_kind(name->payload.str, &align, &has_align);
        if (kind == ATTR_UNKNOWN)
        {
            parse_warning(p, name->loc, "attribute '%s' ignored", name->payload.str);
        }
        if (peek_token(p)->kind == TOK_LPAREN)
        {
            next_token(p);
            if (kind == ATTR_ALIGNED && peek_token(p)->kind == TOK_INT_LIT)
            {
                align = (u64) peek_token(p)->payload.int_val;
                has_align = true;
                next_token(p);
            }
            i32 depth = 1;
            while (depth > 0 && peek_token(p)->kind != TOK_EOF)
            {
                if (peek_token(p)->kind == TOK_LPAREN)
                {
                    depth++;
                }
                else if (peek_token(p)->kind == TOK_RPAREN)
                {
                    depth--;
                }
                next_token(p);
            }
        }
        Attr *a = arena_alloc(p->arena, sizeof(Attr), _Alignof(Attr));
        a->kind = kind;
        a->align = has_align ? align : 0;
        a->name = name->payload.str;
        vec_push(out, a);

        if (peek_token(p)->kind == TOK_COMMA)
        {
            next_token(p);
            continue;
        }
        break;
    }
    if (!expect_token(p, TOK_RPAREN, "')'") || !expect_token(p, TOK_RPAREN, "')'"))
    {
        return false;
    }
    return true;
}

/* Parses every `__attribute__` group at the current position into `*slot`. */
static bool parse_attributes(Parser *p, Vec **slot)
{
    while (is_attribute_name(peek_token(p)))
    {
        if (!*slot)
        {
            *slot = vec_new(p->arena);
        }
        if (!parse_attribute_list(p, *slot))
        {
            return false;
        }
    }
    return true;
}

static bool attrs_has_packed(Vec *attrs)
{
    if (!attrs)
    {
        return false;
    }
    for (size_t i = 0; i < vec_size(attrs); i++)
    {
        if (((Attr *) vec_get(attrs, i))->kind == ATTR_PACKED)
        {
            return true;
        }
    }
    return false;
}

/* The largest `aligned(n)` request (bare `aligned` → 16), or 0. */
static u32 attrs_requested_align(Vec *attrs)
{
    u32 align = 0;
    if (!attrs)
    {
        return 0;
    }
    for (size_t i = 0; i < vec_size(attrs); i++)
    {
        Attr *a = (Attr *) vec_get(attrs, i);
        if (a->kind != ATTR_ALIGNED)
        {
            continue;
        }
        u32 v = a->align ? (u32) a->align : 16;
        if (v > align)
        {
            align = v;
        }
    }
    return align;
}

/* Appends `src` (may be NULL) onto `*dst` (created on demand). */
static void merge_attrs(Vec **dst, Vec *src)
{
    if (!src)
    {
        return;
    }
    if (!*dst)
    {
        *dst = src;
        return;
    }
    for (size_t i = 0; i < vec_size(src); i++)
    {
        vec_push(*dst, vec_get(src, i));
    }
}

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
        bool unnamed = peek_token(p)->kind == TOK_COLON;
        if (unnamed)
        {
            d = (Declarator) {0};
            d.type = base;
            d.name = NULL;
        }
        else if (!parse_declarator(p, base, &d))
        {
            return NULL;
        }

        u32 bit_width = 0;
        if (peek_token(p)->kind == TOK_COLON)
        {
            /* C11 §6.7.2.1p12: `declarator : constant-expression`, integer
               (or _Bool/enum) base, width within the declared type. */
            if (!(type_is_integer(d.type) || d.type->kind == TYPE_ENUM))
            {
                parse_error(p, "bit-field has non-integer type");
                return NULL;
            }
            next_token(p);
            ASTNode *wexpr = parse_assign(p);
            i64 width;
            if (!wexpr || !folded_const(p, wexpr, &width))
            {
                parse_error(p, "bit-field width is not an integer constant expression");
                return NULL;
            }
            u32 maxw = d.type->kind == TYPE_BOOL ? 1 : (u32) (d.type->size * 8);
            if (width <= 0 || (u64) width > maxw)
            {
                parse_error(p, "width of bit-field exceeds its type");
                return NULL;
            }
            bit_width = (u32) width;
        }

        if (!unnamed && !check_not_enumerator(p, d.name))
        {
            return NULL;
        }
        ASTNode *decl = ast_var_decl(d.type, d.name, NULL, SC_NONE, start->loc, p->arena);
        ASTVarDecl *vd = ast_as(ASTVarDecl, decl);
        vd->alignas = alignas;
        vd->bit_width = bit_width;
        merge_attrs(&vd->attrs, d.attrs);
        u32 aattr = attrs_requested_align(vd->attrs);
        if (aattr > vd->alignas)
        {
            vd->alignas = aattr;
        }
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
        rf->bit_offset = -1;
        rf->bit_width = vd->bit_width > 0 ? (i32) vd->bit_width : -1;
        u32 align = attrs_has_packed(vd->attrs) ? 1 : vd->alignas;
        u32 aattr = attrs_requested_align(vd->attrs);
        if (aattr > align)
        {
            align = aattr;
        }
        rf->align_override = align;
        vec_push(record_fields, rf);
    }
}

static bool validate_flexible_members(Parser *p, Type *rec, Vec *fields)
{
    size_t n = vec_size(fields);
    for (size_t i = 0; i < n; i++)
    {
        RecordField *f = (RecordField *) vec_get(fields, i);
        if (type_is_record(f->type) && type_record_has_fam(f->type))
        {
            parse_error(p, "record with a flexible array member cannot be a member");
            return false;
        }
        if (!type_is_array(f->type) || f->type->arr.length != 0 || type_array_is_pending(f->type))
        {
            continue;
        }
        if (rec->kind != TYPE_STRUCT)
        {
            parse_error(p, "flexible array member in a union");
            return false;
        }
        if (i != n - 1)
        {
            parse_error(p, "flexible array member must be the last member");
            return false;
        }
        if (n < 2)
        {
            parse_error(p, "flexible array member in a struct with no other members");
            return false;
        }
        Type *elem = f->type->arr.elem;
        if (elem->kind == TYPE_VOID || type_is_function(elem) || !type_is_complete(elem))
        {
            parse_error(p, "flexible array member has incomplete element type");
            return false;
        }
    }
    return true;
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

        /* Anonymous struct/union member (C11 §6.7.2.1p13): a tagless record
           specifier with no declarator contributes its members to the
           enclosing record. */
        if (type_is_record(mspecs.type) && peek_token(p)->kind == TOK_SEMI &&
            mspecs.type->record.tag == NULL && mspecs.type->record.complete)
        {
            next_token(p);
            ASTNode *anon = ast_var_decl(mspecs.type, NULL, NULL, SC_NONE, mstart->loc, p->arena);
            ast_as(ASTVarDecl, anon)->alignas = mspecs.alignas;
            vec_push(field_decls, anon);
            collect_member_fields(p->arena, record_fields, anon);
            continue;
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
    if (!validate_flexible_members(p, rec, record_fields))
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

        /* C11 requires an enumerator to fit in `int`, but GCC (and C23) accept
           values up to `unsigned int`; the enum then takes an unsigned
           underlying type. Match that so hosted headers (e.g. DWARF/ELF
           constants such as 0xffffffffu) compile. */
        if (value < INT32_MIN || value > 0xffffffffLL)
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
        /* `struct S { ... } __attribute__((packed, aligned(n)))`: apply the
           suffix attributes to the record and re-lay it out. */
        Vec *suffix_attrs = NULL;
        if (!parse_attributes(p, &suffix_attrs))
        {
            return NULL;
        }
        if (suffix_attrs)
        {
            if (attrs_has_packed(suffix_attrs))
            {
                ty->record.packed = true;
            }
            u32 a = attrs_requested_align(suffix_attrs);
            if (a > ty->record.align_override)
            {
                ty->record.align_override = a;
            }
            type_record_relayout(ty);
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
    int n_float = 0;
    int n_double = 0;
    u8 mid_quals = 0;

    Token *t = peek_token(p);
    while (t->kind == TOK_KW_SIGNED || t->kind == TOK_KW_UNSIGNED || t->kind == TOK_KW_CHAR ||
           t->kind == TOK_KW_SHORT || t->kind == TOK_KW_INT || t->kind == TOK_KW_LONG ||
           t->kind == TOK_KW_FLOAT || t->kind == TOK_KW_DOUBLE || t->kind == TOK_KW_CONST ||
           t->kind == TOK_KW_VOLATILE || t->kind == TOK_KW_RESTRICT)
    {
        switch (t->kind)
        {
            case TOK_KW_CONST:
                mid_quals |= Q_CONST;
                break;
            case TOK_KW_VOLATILE:
                mid_quals |= Q_VOLATILE;
                break;
            case TOK_KW_RESTRICT:
                mid_quals |= Q_RESTRICT;
                break;
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
            case TOK_KW_FLOAT:
                n_float++;
                break;
            case TOK_KW_DOUBLE:
                n_double++;
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

    /* FP specifiers: only `float`, `double`, and the recognized `long double`. */
    if (n_float || n_double)
    {
        if (n_signed || n_unsigned || n_char || n_short || n_int)
        {
            parse_error(p, "cannot combine these specifiers with a floating-point type");
            return NULL;
        }
        if (n_float + n_double > 1)
        {
            parse_error(p, "cannot combine 'float' and 'double'");
            return NULL;
        }
        if (n_long > 1)
        {
            parse_error(p, "too many 'long' type specifiers");
            return NULL;
        }
        if (n_float && n_long)
        {
            parse_error(p, "invalid type specifier combination 'long float'");
            return NULL;
        }
        if (n_double && n_long)
        {
            return apply_quals(type_long_double(), mid_quals);
        }
        return apply_quals(n_float ? type_float() : type_double(), mid_quals);
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
        return apply_quals(n_unsigned ? type_uchar() : type_char(), mid_quals);
    }
    if (n_short)
    {
        return apply_quals(n_unsigned ? type_ushort() : type_short(), mid_quals);
    }
    if (n_long)
    {
        if (n_unsigned)
        {
            return apply_quals(n_long >= 2 ? type_ullong() : type_ulong(), mid_quals);
        }
        return apply_quals(n_long >= 2 ? type_llong() : type_long(), mid_quals);
    }
    return apply_quals(n_unsigned ? type_uint() : type_int(), mid_quals);
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
    if (is_typename_start_at(p, skip_quals(p, p->pos)))
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

static u8 qual_bit(TokenKind kind)
{
    switch (kind)
    {
        case TOK_KW_CONST:
            return Q_CONST;
        case TOK_KW_VOLATILE:
            return Q_VOLATILE;
        default:
            return Q_RESTRICT;
    }
}

static bool parse_qualifiers(Parser *p, u8 *quals, u32 *alignas)
{
    for (;;)
    {
        Token *t = peek_token(p);
        if (t->kind == TOK_KW_CONST || t->kind == TOK_KW_VOLATILE || t->kind == TOK_KW_RESTRICT)
        {
            next_token(p);
            *quals |= qual_bit(t->kind);
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
    return true;
}

static Type *apply_quals(Type *t, u8 quals)
{
    if (quals & Q_CONST)
    {
        t = type_const(t);
    }
    if (quals & Q_VOLATILE)
    {
        t = type_volatile(t);
    }
    if (quals & Q_RESTRICT)
    {
        t = type_restrict(t);
    }
    return t;
}

static Specs parse_decl_specifiers(Parser *p)
{
    Specs s = {0};
    if (!parse_attributes(p, &s.attrs))
    {
        return s;
    }
    for (;;)
    {
        TokenKind k = peek_token(p)->kind;
        if (k == TOK_KW_EXTENSION)
        {
            next_token(p);
        }
        else if (k == TOK_KW_INLINE)
        {
            /* Function specifiers (§6.7.4): `inline` drives the tier-1 inliner. */
            s.is_inline = true;
            next_token(p);
        }
        else if (k == TOK_KW_REGISTER || k == TOK_KW_AUTO || k == TOK_KW_NORETURN)
        {
            /* Storage-class/specifier spellings with no effect here. */
            next_token(p);
        }
        else
        {
            break;
        }
    }
    u8 lead_quals = 0;
    if (!parse_qualifiers(p, &lead_quals, &s.alignas))
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
        case TOK_KW_FLOAT:
        case TOK_KW_DOUBLE:
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

    if (lead_quals)
    {
        s.type = apply_quals(s.type, lead_quals);
    }
    u8 trail_quals = 0;
    if (!parse_qualifiers(p, &trail_quals, &s.alignas))
    {
        return s;
    }
    if (trail_quals)
    {
        s.type = apply_quals(s.type, trail_quals);
    }
    if (!parse_attributes(p, &s.attrs))
    {
        return s;
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

/* Pointer layers with per-level qualifiers: the leftmost `*` is the innermost
   (closest to the base), so `T * const *` is a pointer to a const pointer. */
static Type *ptr_layers_quals(Type *t, u32 n, const u8 *quals)
{
    for (u32 i = 0; i < n; i++)
    {
        t = type_ptr(t);
        t = apply_quals(t, quals[i]);
    }
    return t;
}

static Type *parse_group_suffixes(Parser *p, Type *t, u32 *nptr, Vec **captured_params,
                                  bool *captured_variadic)
{
    for (;;)
    {
        TokenKind k = peek_token(p)->kind;
        if (k == TOK_LBRACKET)
        {
            t = parse_array_suffix(p, ptr_layers(t, *nptr), NULL);
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
            /* Keep the params so a `(name)(params){...}` definition can use them. */
            if (captured_params)
            {
                *captured_params = params;
                *captured_variadic = variadic;
            }
            *nptr = 0;
        }
        else
        {
            return t;
        }
    }
}

/* C11 §6.7.6.1: `[N]` binds tighter than `*`, so an outer suffix binds below
   the declarator's own array layers. */
static Type *collect_array_layers(Vec *arrays, Type *t, u32 dims)
{
    for (u32 i = 0; i < dims && t->kind == TYPE_ARRAY; i++)
    {
        vec_push(arrays, t);
        t = t->arr.elem;
    }
    return t;
}

static bool parse_declarator_group(Parser *p, Type *base, u32 nptr, Declarator *out,
                                   bool name_optional)
{
    Declarator inner;
    if (!parse_declarator_core(p, base, &inner, name_optional, true))
    {
        return false;
    }
    if (!expect_token(p, TOK_RPAREN, "')'"))
    {
        return false;
    }

    Vec *arrays = vec_new(p->arena);
    bool inner_is_func = inner.type->kind == TYPE_FUNC;
    Type *core = inner_is_func ? inner.type->func.ret : inner.type;
    core = collect_array_layers(arrays, core, inner.array_dims);
    u32 inner_ptrs = 0;
    /* Strip only the declarator's own `*` layers; pointer layers from the base
       type (e.g. a pointer typedef return type) belong to the return type. */
    while (inner_ptrs < inner.stars && core->kind == TYPE_PTR)
    {
        core = core->ptr.pointee;
        inner_ptrs++;
    }

    Vec *outer_params = NULL;
    bool outer_variadic = false;
    Type *suffix = parse_group_suffixes(p, core, &nptr, &outer_params, &outer_variadic);
    if (!suffix)
    {
        return false;
    }
    Type *result = ptr_layers(ptr_layers(suffix, nptr), inner_ptrs);
    if (inner_is_func)
    {
        result = type_func(result, inner.type->func.params, inner.type->func.is_variadic);
    }
    for (size_t i = vec_size(arrays); i > 0; i--)
    {
        Type *arr = (Type *) vec_get(arrays, i - 1);
        result = type_array(result, arr->arr.length);
    }
    out->type = result;
    out->name = inner.name;
    out->stars = 0;
    out->array_dims = 0;
    out->func_params = inner_is_func ? inner.func_params : outer_params;
    out->func_variadic = inner_is_func ? inner.func_variadic : outer_variadic;
    return true;
}

static Type *parse_abstract_declarator(Parser *p, Type *base)
{
    Declarator d;
    if (!parse_declarator_core(p, base, &d, true, false))
    {
        return NULL;
    }
    return d.type;
}

static bool parse_declarator_core(Parser *p, Type *base, Declarator *out, bool name_optional,
                                  bool inner_group)
{
    out->type = base;
    out->name = NULL;
    out->stars = 0;
    out->array_dims = 0;
    out->func_params = NULL;
    out->func_variadic = false;
    out->attrs = NULL;

    if (!parse_attributes(p, &out->attrs))
    {
        return false;
    }
    enum
    {
        MAX_PTR_LAYERS = 64,
    };
    u32 nptr = 0;
    u8 ptr_layer_quals[MAX_PTR_LAYERS];
    while (peek_token(p)->kind == TOK_STAR)
    {
        next_token(p);
        if (nptr == MAX_PTR_LAYERS)
        {
            parse_error(p, "too many pointer levels");
            return false;
        }
        u8 quals = 0;
        while (peek_token(p)->kind == TOK_KW_CONST || peek_token(p)->kind == TOK_KW_VOLATILE ||
               peek_token(p)->kind == TOK_KW_RESTRICT)
        {
            quals |= qual_bit(peek_token(p)->kind);
            next_token(p);
        }
        ptr_layer_quals[nptr] = quals;
        nptr++;
    }
    out->stars = nptr;

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
        out->type = parse_array_suffix(p, ptr_layers_quals(out->type, nptr, ptr_layer_quals),
                                       &out->array_dims);
        if (!out->type)
        {
            return false;
        }
    }
    else
    {
        out->type = ptr_layers_quals(out->type, nptr, ptr_layer_quals);
    }
    /* A `(params)` suffix inside the group belongs to this declarator. */
    if (inner_group && peek_token(p)->kind == TOK_LPAREN)
    {
        u32 suffix_ptrs = 0;
        out->type = parse_group_suffixes(p, out->type, &suffix_ptrs, &out->func_params,
                                         &out->func_variadic);
        if (!out->type)
        {
            return false;
        }
    }
    return parse_attributes(p, &out->attrs);
}

static bool parse_declarator(Parser *p, Type *base, Declarator *out)
{
    return parse_declarator_core(p, base, out, false, false);
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
    if (!parse_declarator_core(p, s.type, &d, true, false))
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
    ASTVarDecl *vd = ast_as(ASTVarDecl, decl);
    vd->alignas = s.alignas;
    merge_attrs(&vd->attrs, s.attrs);
    merge_attrs(&vd->attrs, d.attrs);
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

static Type *parse_array_suffix(Parser *p, Type *type, u32 *ndim_out)
{
    enum
    {
        MAX_ARRAY_DIM = 32,
    };
    u64 dims[MAX_ARRAY_DIM];
    ASTNode *dim_exprs[MAX_ARRAY_DIM];
    size_t ndim = 0;
    while (peek_token(p)->kind == TOK_LBRACKET)
    {
        next_token(p);
        while (peek_token(p)->kind == TOK_KW_CONST || peek_token(p)->kind == TOK_KW_VOLATILE ||
               peek_token(p)->kind == TOK_KW_RESTRICT)
        {
            next_token(p);
        }
        if (peek_token(p)->kind == TOK_KW_STATIC)
        {
            next_token(p);
            while (peek_token(p)->kind == TOK_KW_CONST || peek_token(p)->kind == TOK_KW_VOLATILE ||
                   peek_token(p)->kind == TOK_KW_RESTRICT)
            {
                next_token(p);
            }
        }
        u64 len = 0;
        ASTNode *bound_expr = NULL;
        if (peek_token(p)->kind == TOK_STAR)
        {
            next_token(p);
        }
        else if (peek_token(p)->kind != TOK_RBRACKET)
        {
            ASTNode *size_expr = parse_assign(p);
            if (!size_expr)
            {
                return NULL;
            }
            i64 folded;
            if (folded_const(p, size_expr, &folded))
            {
                len = (u64) folded;
            }
            else
            {
                bound_expr = size_expr;
            }
        }
        expect_token(p, TOK_RBRACKET, "]");
        if (ndim == MAX_ARRAY_DIM)
        {
            parse_error(p, "too many array dimensions");
            return NULL;
        }
        dims[ndim] = len;
        dim_exprs[ndim] = bound_expr;
        ndim++;
    }
    for (size_t i = ndim; i > 0; i--)
    {
        if (dim_exprs[i - 1])
        {
            type = type_array_pending(type, dim_exprs[i - 1]);
        }
        else
        {
            type = type_array(type, dims[i - 1]);
        }
    }
    if (ndim_out)
    {
        *ndim_out = (u32) ndim;
    }
    return type;
}

typedef struct StoragePrefix
{
    StorageClass storage;
    u8 lead_quals; /* qualifiers consumed ahead of the storage class */
} StoragePrefix;

static StoragePrefix parse_storage_prefix(Parser *p)
{
    StoragePrefix pre = {SC_NONE, 0};
    Token *t = peek_token(p);
    if (t->kind != TOK_KW_CONST && t->kind != TOK_KW_VOLATILE && t->kind != TOK_KW_RESTRICT &&
        t->kind != TOK_KW_STATIC && t->kind != TOK_KW_EXTERN)
    {
        return pre;
    }

    u8 lead = 0;
    size_t i = 0;
    for (;; i++)
    {
        TokenKind k = (p->pos + i < p->count) ? p->tokens[p->pos + i].kind : TOK_EOF;
        if (k == TOK_KW_CONST || k == TOK_KW_VOLATILE || k == TOK_KW_RESTRICT)
        {
            lead |= k == TOK_KW_CONST ? Q_CONST : k == TOK_KW_VOLATILE ? Q_VOLATILE : Q_RESTRICT;
        }
        else if (k == TOK_KW_STATIC || k == TOK_KW_EXTERN)
        {
            break;
        }
        else
        {
            return pre;
        }
    }
    for (size_t k = 0; k < i; k++)
    {
        next_token(p);
    }
    TokenKind nxt = (p->pos < p->count) ? p->tokens[p->pos].kind : TOK_EOF;
    next_token(p);
    pre.storage = nxt == TOK_KW_STATIC ? SC_STATIC : SC_EXTERN;
    pre.lead_quals = lead;
    return pre;
}

static Specs parse_specs_storage(Parser *p, bool storage_ok)
{
    /* A GNU attribute may precede the storage class (`__attribute__((x)) static
       int f;`); collect it before the storage prefix consumes `static`. */
    Vec *lead_attrs = NULL;
    if (!parse_attributes(p, &lead_attrs))
    {
        return (Specs) {0};
    }

    u8 lead = 0;
    StorageClass storage = SC_NONE;
    if (storage_ok)
    {
        StoragePrefix pre = parse_storage_prefix(p);
        storage = pre.storage;
        lead = pre.lead_quals;
    }

    Specs s = parse_decl_specifiers(p);
    merge_attrs(&s.attrs, lead_attrs);
    s.type = apply_quals(s.type, lead);
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

    Vec *decls = vec_new(p->arena);
    while (true)
    {
        Declarator d;
        if (!parse_declarator(p, s.type, &d))
        {
            return NULL;
        }
        if (peek_token(p)->kind == TOK_LPAREN)
        {
            push_scope(p);
            u32 suffix_ptrs = 0;
            d.type =
                parse_group_suffixes(p, d.type, &suffix_ptrs, &d.func_params, &d.func_variadic);
            pop_scope(p);
            if (!d.type)
            {
                return NULL;
            }
        }
        if (!check_not_enumerator(p, d.name))
        {
            return NULL;
        }
        if (!declare_name(p, d.name, BIND_TYPEDEF, d.type))
        {
            return NULL;
        }
        vec_push(decls, ast_typedef_decl(d.type, d.name, start->loc, p->arena));
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
    merge_attrs(&vd->attrs, s.attrs);
    merge_attrs(&vd->attrs, d.attrs);
    u32 aattr = attrs_requested_align(vd->attrs);
    if (aattr > vd->alignas)
    {
        vd->alignas = aattr;
    }
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
    FuncSpecs fs = {.storage = storage, .is_inline = s.is_inline};
    merge_attrs(&fs.attrs, s.attrs);
    merge_attrs(&fs.attrs, d.attrs);
    if (!check_not_enumerator(p, d.name))
    {
        return NULL;
    }
    if (!declare_name(p, d.name, BIND_FUNC, NULL))
    {
        return NULL;
    }

    next_token(p);
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
    if (!parse_attributes(p, &fs.attrs))
    {
        return NULL;
    }

    if (peek_token(p)->kind == TOK_SEMI)
    {
        next_token(p);
        pop_scope(p);
        return ast_func_decl(d.type, d.name, params, fs, variadic, start, p->arena);
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

    const char *saved_func = p->cur_func;
    p->cur_func = d.name;
    ASTNode *body = parse_compound_stmt(p);
    p->cur_func = saved_func;
    if (!body)
    {
        return NULL;
    }
    pop_scope(p);
    return ast_func_def(d.type, d.name, params, body, fs, variadic, start, p->arena);
}

static ASTNode *parse_func_from_type(Parser *p, Specs s, Declarator d, Loc start)
{
    /* The parenthesized declarator already folded the params; only `;` or `{...}` remains. */
    if (s.alignas)
    {
        parse_error(p, "_Alignas is not permitted on a function");
        return NULL;
    }
    StorageClass storage = s.storage == SC_STATIC ? SC_STATIC : SC_NONE;
    FuncSpecs fs = {.storage = storage, .is_inline = s.is_inline};
    merge_attrs(&fs.attrs, s.attrs);
    merge_attrs(&fs.attrs, d.attrs);
    if (!check_not_enumerator(p, d.name))
    {
        return NULL;
    }
    if (!declare_name(p, d.name, BIND_FUNC, NULL))
    {
        return NULL;
    }
    if (!parse_attributes(p, &fs.attrs))
    {
        return NULL;
    }
    bool is_definition = peek_token(p)->kind == TOK_LBRACE;

    Vec *params;
    if (d.func_params)
    {
        params = d.func_params;
    }
    else
    {
        /* Rebuild the parameters from the type, unattached to names. */
        params = vec_new(p->arena);
        Vec *ptypes = d.type->func.params;
        for (size_t i = 0; i < vec_size(ptypes); i++)
        {
            Type *pt = type_unqual((Type *) vec_get(ptypes, i));
            vec_push(params, ast_var_decl(type_decay(pt), NULL, NULL, SC_NONE, start, p->arena));
        }
    }

    if (!is_definition)
    {
        if (!expect_token(p, TOK_SEMI, "';'"))
        {
            return NULL;
        }
        return ast_func_decl(d.type->func.ret, d.name, params, fs,
                             d.type->func.is_variadic || d.func_variadic, start, p->arena);
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
    push_scope(p);
    const char *saved_func = p->cur_func;
    p->cur_func = d.name;
    ASTNode *body = parse_compound_stmt(p);
    p->cur_func = saved_func;
    if (!body)
    {
        return NULL;
    }
    pop_scope(p);
    return ast_func_def(d.type->func.ret, d.name, params, body, fs,
                        d.type->func.is_variadic || d.func_variadic, start, p->arena);
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
    if (d.type->kind == TYPE_FUNC)
    {
        return parse_func_from_type(p, s, d, start->loc);
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
        if (is_typename_start(p) || peek_token(p)->kind == TOK_KW_CONST ||
            peek_token(p)->kind == TOK_KW_VOLATILE || peek_token(p)->kind == TOK_KW_RESTRICT)
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
    next_token(p);
    next_token(p);
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
    if (t->kind == TOK_SEMI)
    {
        /* C11 §6.8.3: null statement; macro bodies leave a stray `;`. */
        next_token(p);
        return ast_compound_stmt(vec_new(p->arena), t->loc, p->arena);
    }
    if (is_type_start(t->kind) || t->kind == TOK_KW_ALIGNAS || t->kind == TOK_KW_CONST ||
        t->kind == TOK_KW_VOLATILE || t->kind == TOK_KW_RESTRICT || t->kind == TOK_KW_STATIC ||
        t->kind == TOK_KW_EXTERN || t->kind == TOK_KW_REGISTER || t->kind == TOK_KW_AUTO ||
        t->kind == TOK_KW_INLINE || t->kind == TOK_KW_NORETURN || t->kind == TOK_KW_EXTENSION)
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
    if (!is_typename_start_at(p, skip_quals(p, p->pos)))
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

static ASTNode *parse_generic_selection(Parser *p, Loc loc)
{
    next_token(p);
    if (!expect_token(p, TOK_LPAREN, "'('"))
    {
        return NULL;
    }
    ASTNode *controlling = parse_assign(p);
    if (!controlling)
    {
        return NULL;
    }
    if (!expect_token(p, TOK_COMMA, "','"))
    {
        return NULL;
    }
    Vec *assocs = vec_new(p->arena);
    ASTNode *default_expr = NULL;
    for (;;)
    {
        if (peek_token(p)->kind == TOK_KW_DEFAULT)
        {
            next_token(p);
            if (default_expr)
            {
                parse_error(p, "duplicate 'default' association in _Generic");
                return NULL;
            }
            if (!expect_token(p, TOK_COLON, "':'"))
            {
                return NULL;
            }
            default_expr = parse_assign(p);
            if (!default_expr)
            {
                return NULL;
            }
        }
        else
        {
            Type *type = parse_type_name(p);
            if (!type)
            {
                return NULL;
            }
            if (!expect_token(p, TOK_COLON, "':'"))
            {
                return NULL;
            }
            ASTNode *expr = parse_assign(p);
            if (!expr)
            {
                return NULL;
            }
            GenericAssoc *assoc = arena_alloc(p->arena, sizeof(GenericAssoc), sizeof(void *));
            assoc->type = type;
            assoc->expr = expr;
            vec_push(assocs, assoc);
        }
        if (peek_token(p)->kind != TOK_COMMA)
        {
            break;
        }
        next_token(p);
    }
    if (!expect_token(p, TOK_RPAREN, "')'"))
    {
        return NULL;
    }
    return ast_generic_selection(controlling, assocs, default_expr, loc, p->arena);
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
        case TOK_FLOAT_LIT:
            next_token(p);
            {
                ASTFloatValue v;
                if (t->float_kind == FK_LONG)
                {
                    v.ld = t->payload.ld_val;
                }
                else
                {
                    v.bits = t->payload.float_pat;
                }
                return ast_float_literal(t->float_kind, v, t->loc, p->arena);
            }
        case TOK_CHAR_LIT:
            next_token(p);
            return ast_int_literal(t->payload.int_val, false, SUFFIX_NONE, false, t->loc, p->arena);
        case TOK_IDENT:
            if (strcmp(t->payload.str, "__builtin_va_arg") == 0 && p->pos + 1 < p->count &&
                p->tokens[p->pos + 1].kind == TOK_LPAREN)
            {
                return parse_builtin_va_arg(p, t);
            }
            if (strcmp(t->payload.str, "__func__") == 0 && p->cur_func)
            {
                next_token(p);
                return ast_string_literal(p->cur_func, (u64) strlen(p->cur_func), t->loc, p->arena);
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
            return ast_string_literal_kind(t->payload.str, t->str_len, t->str_kind, t->loc,
                                           p->arena);
        case TOK_KW_GENERIC:
            return parse_generic_selection(p, t->loc);
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

static Type *parse_type_name(Parser *p)
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
    return parse_array_suffix(p, ty, NULL);
}

static Type *parse_paren_type_name(Parser *p)
{
    Type *ty = parse_type_name(p);
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
        next_token(p);
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
            next_token(p);
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
            next_token(p);
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

    if (t->kind == TOK_KW_EXTENSION)
    {
        next_token(p);
        return parse_unary(p);
    }
    if (t->kind == TOK_PLUS)
    {
        /* Unary plus is the identity (after integer promotion). */
        next_token(p);
        return parse_unary(p);
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
            i64 folded;
            if (!idx || !folded_const(p, idx, &folded))
            {
                parse_error(p, "array designator index must be an integer constant");
                return NULL;
            }
            if (!expect_token(p, TOK_RBRACKET, "']'"))
            {
                return NULL;
            }
            d->kind = ND_INDEX;
            d->index = folded;
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

/* The promoted type of a foldable constant expression, derived from the
   leaves (literal suffixes, casts, sizeof) because parse-time nodes have no
   expr_type yet. Lets `folded_const` honor unsigned DIV/REM/SHR and
   relational semantics (§6.3.1.8). */
static bool folded_const(Parser *p, ASTNode *node, i64 *out);
static Type *folded_const_type(Parser *p, ASTNode *node)
{
    if (!node)
    {
        return type_int();
    }
    switch (node->kind)
    {
        case AST_INT_LITERAL:
        {
            ASTIntLiteral *lit = ast_as(ASTIntLiteral, node);
            return type_int_literal(lit->value, lit->is_hex, lit->is_unsigned, lit->length);
        }
        case AST_UNARY_EXPR:
        {
            ASTUnaryExpr *u = ast_as(ASTUnaryExpr, node);
            return u->op == UN_LOG_NOT ? type_int() : folded_const_type(p, u->operand);
        }
        case AST_BINARY_EXPR:
        {
            ASTBinaryExpr *b = ast_as(ASTBinaryExpr, node);
            Type *lt = folded_const_type(p, b->left);
            Type *rt = folded_const_type(p, b->right);
            if (b->op == BIN_SHL || b->op == BIN_SHR)
            {
                return type_promote(lt);
            }
            switch (b->op)
            {
                case BIN_LOG_AND:
                case BIN_LOG_OR:
                case BIN_EQ:
                case BIN_NE:
                case BIN_LT:
                case BIN_GT:
                case BIN_LE:
                case BIN_GE:
                    return type_int();
                default:
                    return type_common(type_promote(lt), type_promote(rt));
            }
        }
        case AST_TERNARY_EXPR:
        {
            ASTTernaryExpr *te = ast_as(ASTTernaryExpr, node);
            i64 cond;
            if (!folded_const(p, te->cond, &cond))
            {
                return type_int();
            }
            return folded_const_type(p, cond ? te->then_expr : te->else_expr);
        }
        case AST_SIZEOF_TYPE:
        case AST_SIZEOF_EXPR:
        case AST_ALIGNOF_TYPE:
        case AST_ALIGNOF_EXPR:
            return type_ulong(); /* size_t: results are unsigned */
        case AST_CAST_EXPR:
            return type_is_integer(ast_as(ASTCastExpr, node)->target_type)
                       ? type_unqual(ast_as(ASTCastExpr, node)->target_type)
                       : type_int();
        default:
            return type_int();
    }
}

/* Whether a folded binary operation follows unsigned semantics: the usual
   arithmetic conversions' common type for DIV/REM and the relational
   comparands, the promoted left operand for shifts. */
static bool folded_binary_unsigned(Parser *p, ASTBinaryExpr *b)
{
    if (b->op == BIN_SHL || b->op == BIN_SHR)
    {
        return type_is_unsigned(type_promote(folded_const_type(p, b->left)));
    }
    Type *lt = type_promote(folded_const_type(p, b->left));
    Type *rt = type_promote(folded_const_type(p, b->right));
    return type_is_unsigned(type_common(lt, rt));
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
            if (u->op == UN_ADDR && u->operand->kind == AST_MEMBER_ACCESS)
            {
                /* `offsetof` reads the member's byte offset inside the record (§7.19p3). */
                ASTMemberAccess *ma = ast_as(ASTMemberAccess, u->operand);
                if (ma->object->kind == AST_CAST_EXPR)
                {
                    ASTCastExpr *ce = ast_as(ASTCastExpr, ma->object);
                    i64 base;
                    if (ce->target_type && ce->target_type->kind == TYPE_PTR &&
                        type_is_record(ce->target_type->ptr.pointee) && ce->operand &&
                        folded_const(p, ce->operand, &base) && base == 0)
                    {
                        *out = (i64) type_record_field_offset(ce->target_type->ptr.pointee,
                                                              ma->member);
                        return true;
                    }
                }
                return false;
            }
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
                    if (folded_binary_unsigned(p, b))
                    {
                        *out = b->op == BIN_DIV ? (i64) ((u64) l / (u64) r)
                                                : (i64) ((u64) l % (u64) r);
                    }
                    else
                    {
                        *out = b->op == BIN_DIV ? l / r : l % r;
                    }
                    return true;
                case BIN_SHL:
                case BIN_SHR:
                    if (r < 0 || r >= (i64) (sizeof(i64) * 8))
                    {
                        return false;
                    }
                    if (b->op == BIN_SHR && folded_binary_unsigned(p, b))
                    {
                        *out = (i64) ((u64) l >> r); /* logical */
                    }
                    else if (b->op == BIN_SHR)
                    {
                        *out = l >> r; /* arithmetic */
                    }
                    else
                    {
                        *out = (i64) ((u64) l << r);
                    }
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
                    *out = folded_binary_unsigned(p, b) ? (i64) ((u64) l < (u64) r) : l < r;
                    return true;
                case BIN_GT:
                    *out = folded_binary_unsigned(p, b) ? (i64) ((u64) l > (u64) r) : l > r;
                    return true;
                case BIN_LE:
                    *out = folded_binary_unsigned(p, b) ? (i64) ((u64) l <= (u64) r) : l <= r;
                    return true;
                case BIN_GE:
                    *out = folded_binary_unsigned(p, b) ? (i64) ((u64) l >= (u64) r) : l >= r;
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
        case AST_SIZEOF_EXPR:
            /* sizeof of a string literal is the array size incl. the NUL. */
            if (ast_as(ASTSizeofExpr, node)->operand &&
                ast_as(ASTSizeofExpr, node)->operand->kind == AST_STRING_LITERAL)
            {
                ASTStringLiteral *sl =
                    ast_as(ASTStringLiteral, ast_as(ASTSizeofExpr, node)->operand);
                *out = (i64) (sl->length + str_kind_elem_size(sl->str_kind));
                return true;
            }
            return false;
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
    if (expr->kind == AST_CAST_EXPR)
    {
        /* Null pointer constant spelled with a cast, e.g. `(void*)0` (§6.3.2.3p3). */
        ASTCastExpr *ce = ast_as(ASTCastExpr, expr);
        i64 v;
        if (ce->operand && folded_const(p, ce->operand, &v) && v == 0)
        {
            vd->const_init = 0;
            vd->has_const_init = true;
            return true;
        }
    }
    /* FP scalars defer to the init planner (ir_builder owns the single FP fold). */
    if (type_is_fp(type_unqual(vd->type)))
    {
        vd->init = expr;
        return true;
    }
    i64 value;
    if (folded_const(p, expr, &value))
    {
        vd->const_init = value;
        vd->has_const_init = true;
        return true;
    }
    /* Not foldable without types (e.g. `sizeof(x)`): defer to semantic, which
       folds it as an integer constant expression or rejects it. */
    vd->init = expr;
    return true;
}

ASTNode *parse(Token *tokens, u64 count, const ParserConfig *cfg, Arena *arena)
{
    ASSERT(count > 0);
    ParserConfig pc = (ParserConfig) {0};
    if (cfg)
    {
        pc = *cfg;
    }
    Parser p = {tokens, count, 0, arena, strmap_new(arena), vec_new(arena), pc, NULL};
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
