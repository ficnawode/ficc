#include "parser.h"
#include "util/hashmap.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct ParserCtx ParserCtx;
struct ParserCtx
{
    Token *tokens;
    u64 count;
    u64 pos;
    Arena *arena;
    StrMap *enum_consts; /* enumerator name -> i64* value, resolved at parse time */
    Vec *name_scopes;    /* Vec<StrMap*> — ordinary-name bindings (name -> ParserBinding*) */
};

/* Ordinary identifiers live in one namespace (C11 §6.2.3): variables,
   functions, and typedef names. The parser tracks a scoped table of bindings
   (D12.1/D12.2) so it can tell — while parsing — whether an identifier is a
   type name: cast disambiguation, `sizeof(T)`, and declaration specifiers are
   all parse-time decisions. Scope discipline mirrors semantic's: one file
   scope, one per compound statement, one per function (params + body). */
typedef enum
{
    BIND_TYPEDEF,
    BIND_VAR,
    BIND_FUNC,
} ParserBindingKind;

typedef struct ParserBinding ParserBinding;
struct ParserBinding
{
    ParserBindingKind kind;
    Type *type; /* only meaningful for BIND_TYPEDEF */
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

static StrMap *current_name_scope(ParserCtx *p)
{
    return (StrMap *) vec_last(p->name_scopes);
}

static void push_name_scope(ParserCtx *p)
{
    vec_push(p->name_scopes, strmap_new(p->arena));
}

static void pop_name_scope(ParserCtx *p)
{
    (void) vec_pop(p->name_scopes);
}

/* Walk the name-scope stack innermost-first. */
static ParserBinding *name_lookup(ParserCtx *p, const char *name)
{
    size_t n = vec_size(p->name_scopes);
    for (size_t i = n; i > 0; i--)
    {
        ParserBinding *b = strmap_get((StrMap *) vec_get(p->name_scopes, i - 1), name);
        if (b)
        {
            return b;
        }
    }
    return NULL;
}

/* Declare an ordinary name in the current scope, enforcing the C11 §6.2.3
   rule that a scope holds one binding per name. A typedef may not redeclare a
   name that exists in the same scope, and no name may hide a typedef in the
   same scope; the one legal same-scope repeat is a typedef redefined to the
   *same* (interned) type (§6.7: "may be redeclared to refer to the same type").
   Var/var and func/func repeats are left to semantic, which owns the finer
   merging logic (extern/tentative definitions). */
static bool name_declare(ParserCtx *p, const char *name, ParserBindingKind kind, Type *type)
{
    ParserBinding *existing = strmap_get(current_name_scope(p), name);
    if (existing)
    {
        if (existing->kind == BIND_TYPEDEF && kind == BIND_TYPEDEF)
        {
            if (existing->type != type)
            {
                parser_error(p, "typedef '%s' redefined with a different type", name);
                return false;
            }
            return true;
        }
        if (existing->kind == BIND_TYPEDEF || kind == BIND_TYPEDEF)
        {
            parser_error(p, "'%s' redeclared as a different kind of symbol", name);
            return false;
        }
        return true;
    }
    ParserBinding *b = arena_alloc(p->arena, sizeof(ParserBinding), sizeof(void *));
    b->kind = kind;
    b->type = type;
    strmap_set(current_name_scope(p), name, b);
    return true;
}

static Type *parse_type_specifier(ParserCtx *p);
static Type *parse_abstract_declarator(ParserCtx *p, Type *base);
static Type *parse_array_suffix(ParserCtx *p, Type *type);

/* Declaration specifiers: the base type of a declaration plus the tag-
   definition node produced by an inline *tagged* struct/union/enum
   definition (`struct S { ... }` / `enum E { ... }`), or NULL. A caller
   emits `tag_def` only for a declaration with zero declarators
   (`struct S { ... };`); a combined `struct S { ... } v;` drops it — the
   type is complete the moment the specifier returns, so the declarator
   carries it. Anonymous definitions never produce a node (the declarator or
   typedef name is the only witness). */
typedef struct DeclSpecifiers
{
    Type *type;
    ASTNode *tag_def;
    u32 alignas; /* requested alignment from `_Alignas(...)`, 0 = natural
                    (D14.6: accepted and validated; honored up to the object's
                    natural alignment only) */
} DeclSpecifiers;

static DeclSpecifiers parse_decl_specifiers(ParserCtx *p);
static ASTNode *parse_expression(ParserCtx *p);
static ASTNode *parse_stmt(ParserCtx *p);
static ASTNode *parse_primary(ParserCtx *p);
static ASTNode *parse_postfix_ops(ParserCtx *p, ASTNode *node);
static ASTNode *parse_switch_stmt(ParserCtx *p);
static ASTNode *parse_case_stmt(ParserCtx *p);
static ASTNode *parse_default_stmt(ParserCtx *p);
static bool fold_constant_expr(ParserCtx *p, ASTNode *node, i64 *out);
static bool parser_check_not_enumerator(ParserCtx *p, const char *name);
static ASTNode *parse_unary(ParserCtx *p);
static ASTNode *parse_typedef_decl(ParserCtx *p);
static Vec *parse_record_body(ParserCtx *p, Type *rec);
static bool parse_enumerator_body(ParserCtx *p, Vec *constants, i64 *next_value);
static bool parse_declarator(ParserCtx *p, Type *base, Type **out_type, const char **out_name);
static bool resolve_constant_init(ParserCtx *p, ASTVarDecl *vd, ASTNode *expr);

/* The token kinds that can begin a type specifier. Used to disambiguate a
   cast `(type)expr` from a parenthesized expression: a cast must open with
   one of these (after any leading `const`) or a visible typedef identifier
   (D12.3). `is_typename_start[_at]` is the parser-contextual form that
   includes typedef names; a plain identifier not bound to a typedef never
   starts a typename, so `(a)`/`(a+b)`/`f(x)` stay paren expressions/calls. */
static bool is_type_start(TokenKind k)
{
    return k == TOK_KW_INT || k == TOK_KW_BOOL || k == TOK_KW_CHAR || k == TOK_KW_SHORT ||
           k == TOK_KW_LONG || k == TOK_KW_UNSIGNED || k == TOK_KW_SIGNED || k == TOK_KW_VOID ||
           k == TOK_KW_STRUCT || k == TOK_KW_UNION || k == TOK_KW_ENUM;
}

static bool is_typename_start_at(ParserCtx *p, size_t pos)
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
        ParserBinding *b = name_lookup(p, t->payload.str);
        return b && b->kind == BIND_TYPEDEF;
    }
    return false;
}

/* Index of the first non-`const` token at or after `pos` (typedef-qualified
   type names and casts may open with `const`). */
static size_t skip_const_ahead(ParserCtx *p, size_t pos)
{
    while (pos < p->count && p->tokens[pos].kind == TOK_KW_CONST)
    {
        pos++;
    }
    return pos;
}

static bool is_typename_start(ParserCtx *p)
{
    return is_typename_start_at(p, p->pos);
}
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
static ASTNode *parse_initializer(ParserCtx *p);
static ASTNode *parse_init_list(ParserCtx *p);

/* A struct/union member `type-specifier declarator-list ;` — the field-
   declarator list (§6.7.6, field-declaration in §6.7.2.1p8). Supports
   multi-declarators (`int a, b;`) and inline/definitions of nested records.
   Returns a single ASTVarDecl or an AST_DECL_LIST. */
static ASTNode *parse_member_decl_body(ParserCtx *p, Type *base, u32 alignas, Token *start)
{
    if (parser_peek(p)->kind == TOK_SEMI)
    {
        parser_error(p, "member declaration must declare a member");
        return NULL;
    }

    Vec *decls = vec_new(p->arena);
    while (true)
    {
        Type *dtype;
        const char *name;
        if (!parse_declarator(p, base, &dtype, &name))
        {
            return NULL;
        }
        if (!parser_check_not_enumerator(p, name))
        {
            return NULL;
        }
        ASTNode *decl = ast_var_decl(dtype, name, NULL, SC_NONE, start->loc, p->arena);
        ast_as(ASTVarDecl, decl)->alignas = alignas;
        vec_push(decls, decl);
        if (parser_peek(p)->kind != TOK_COMMA)
        {
            break;
        }
        parser_advance(p);
    }
    if (!parser_expect(p, TOK_SEMI, "';'"))
    {
        return NULL;
    }
    if (vec_size(decls) == 1)
    {
        return (ASTNode *) vec_get(decls, 0);
    }
    return ast_decl_list(decls, start->loc, p->arena);
}

/* Unfolds a member ASTNode (VARDECL or DECL_LIST) into the record's
   field-layout table. */
static void push_member_fields(Arena *arena, Vec *record_fields, ASTNode *member)
{
    if (member->kind == AST_DECL_LIST)
    {
        ASTDeclList *dl = ast_as(ASTDeclList, member);
        size_t n = vec_size(dl->decls);
        for (size_t i = 0; i < n; i++)
        {
            ASTVarDecl *vd = ast_as(ASTVarDecl, (ASTNode *) vec_get(dl->decls, i));
            RecordField *rf = arena_alloc(arena, sizeof(RecordField), _Alignof(RecordField));
            rf->name = vd->name;
            rf->type = vd->type;
            rf->offset = 0;
            vec_push(record_fields, rf);
        }
        return;
    }
    ASTVarDecl *vd = ast_as(ASTVarDecl, member);
    RecordField *rf = arena_alloc(arena, sizeof(RecordField), _Alignof(RecordField));
    rf->name = vd->name;
    rf->type = vd->type;
    rf->offset = 0;
    vec_push(record_fields, rf);
}

/* A struct/union definition `{ member-or-declarator-list }`. Members are
   parsed from the shared declaration machinery (multi-declarator lists are
   legal: `struct S { int a, b; };`). The record is completed here, at parse
   time, exactly like file-scope definitions always were — the type registry
   is downstream's only witness. Returns the flattened member list (for the
   tag-def AST node's dump). */
static Vec *parse_record_body(ParserCtx *p, Type *rec)
{
    if (!parser_expect(p, TOK_LBRACE, "'{'"))
    {
        return NULL;
    }

    Vec *field_decls = vec_new(p->arena);   /* Vec<ASTNode*>: VAR_DECL or DECL_LIST */
    Vec *record_fields = vec_new(p->arena); /* Vec<RecordField*> */
    while (parser_peek(p)->kind != TOK_RBRACE)
    {
        Token *mstart = parser_peek(p);
        DeclSpecifiers mspecs = parse_decl_specifiers(p);
        if (!mspecs.type)
        {
            return NULL;
        }

        ASTNode *member = parse_member_decl_body(p, mspecs.type, mspecs.alignas, mstart);
        if (!member)
        {
            return NULL;
        }
        vec_push(field_decls, member);
        push_member_fields(p->arena, record_fields, member);
    }
    if (!parser_expect(p, TOK_RBRACE, "'}'"))
    {
        return NULL;
    }
    type_record_complete(rec, record_fields);
    return field_decls;
}

/* `{ enumerator = const, ... }` of an enum definition: registers every
   enumerator in the parser's enum-constant table (visible in expressions
   from here on, parse time) and fills `constants`. */
static bool parse_enumerator_body(ParserCtx *p, Vec *constants, i64 *next_value)
{
    if (!parser_expect(p, TOK_LBRACE, "'{'"))
    {
        return false;
    }

    while (parser_peek(p)->kind != TOK_RBRACE)
    {
        Token *name_tok = parser_peek(p);
        if (name_tok->kind != TOK_IDENT)
        {
            parser_error(p, "expected enumerator name");
            return false;
        }
        parser_advance(p);
        const char *name = name_tok->payload.str;

        if (strmap_get(p->enum_consts, name))
        {
            parser_error(p, "redefinition of enumerator '%s'", name);
            return false;
        }

        i64 value = *next_value; /* auto-increment (C11 §6.7.2.2p3) */
        if (parser_peek(p)->kind == TOK_ASSIGN)
        {
            parser_advance(p);
            ASTNode *init = parse_assign(p);
            if (!init)
            {
                return false;
            }
            if (!fold_constant_expr(p, init, &value))
            {
                parser_error(p, "enumerator value is not an integer constant expression");
                return false;
            }
        }

        if (value < INT32_MIN || value > INT32_MAX)
        {
            parser_error(p, "enumerator value out of range (must fit in int)");
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

        TokenKind sep = parser_peek(p)->kind;
        if (sep == TOK_COMMA)
        {
            parser_advance(p);
        }
        else if (sep != TOK_RBRACE)
        {
            parser_error(p, "expected ',' or '}' in enum declaration");
            return false;
        }
    }
    return parser_expect(p, TOK_RBRACE, "'}'");
}

/* `struct`/`union` type specifier: optionl tag, optional inline definition
   (completes the type immediately), or a reference. Anonymous definitions
   create a fresh interned type (C11: each is distinct). */
static Type *parse_record_specifier(ParserCtx *p, bool is_union, ASTNode **tag_def)
{
    Token *kw = parser_peek(p);
    ASSERT(kw->kind == TOK_KW_STRUCT || kw->kind == TOK_KW_UNION);
    parser_advance(p);
    TypeKind kind = is_union ? TYPE_UNION : TYPE_STRUCT;

    const char *tag = NULL;
    Type *ty = NULL;
    Token *nt = parser_peek(p);
    if (nt->kind == TOK_IDENT)
    {
        tag = nt->payload.str;
        parser_advance(p);
        Type *existing = type_record_lookup(tag);
        if (existing && existing->kind != kind)
        {
            parser_error(p, "tag '%s' redeclared with a different kind", tag);
            return NULL;
        }
        ty = type_record(kind, tag); /* register incomplete before members (self-ref) */
        nt = parser_peek(p);
    }

    if (nt->kind == TOK_LBRACE)
    {
        if (ty && ty->record.complete)
        {
            parser_error(p, "redefinition of '%s'", tag);
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
        /* A reference to an existing or forward-declared record. When a bare
           `struct TAG;` follows, the top-level/statement `;` path emits the
           (empty-fields) tag-def node for the declaration. */
        *tag_def = ast_struct_decl(tag, is_union, vec_new(p->arena), kw->loc, p->arena);
        return ty;
    }
    parser_error(p, "expected tag name or '{' after '%s'", is_union ? "union" : "struct");
    return NULL;
}

/* `enum` type specifier: optional tag, optional inline definition (registers
   the enumerators, completes the type), or a reference to a defined enum. */
static Type *parse_enum_specifier(ParserCtx *p, ASTNode **tag_def)
{
    Token *kw = parser_peek(p);
    ASSERT(kw->kind == TOK_KW_ENUM);
    parser_advance(p);

    const char *tag = NULL;
    Type *ty = NULL;
    Token *nt = parser_peek(p);
    if (nt->kind == TOK_IDENT)
    {
        tag = nt->payload.str;
        parser_advance(p);
        Type *existing = type_record_lookup(tag);
        if (existing && existing->kind != TYPE_ENUM)
        {
            parser_error(p, "tag '%s' redeclared with a different kind", tag);
            return NULL;
        }
        ty = type_enum(tag);
        nt = parser_peek(p);
    }

    if (nt->kind == TOK_LBRACE)
    {
        if (ty && ty->enumm.complete)
        {
            parser_error(p, "redefinition of '%s'", tag);
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
        /* Definitions always produce a node so a bare `enum { ... };`
           declaration has something to return; anonymous tags go out as a
           tag-less AST_ENUM_DECL (downstream no-op). */
        *tag_def = ast_enum_decl(tag, constants, kw->loc, p->arena);
        return ty;
    }

    if (tag)
    {
        return ty; /* reference to a defined enum */
    }
    parser_error(p, "expected '{' after enum tag; enum types cannot be incomplete");
    return NULL;
}

/* Maps a sequence of integer type-specifier keywords to a Type (C11 §6.7.2
   int/char/signed/unsigned/short/long combinations). Ficc's plain `char` is a
   signed 8-bit type, so `signed char` is the same Type as `char`. */
static Type *parse_integer_specifiers(ParserCtx *p)
{
    int n_signed = 0;
    int n_unsigned = 0;
    int n_char = 0;
    int n_short = 0;
    int n_int = 0;
    int n_long = 0;

    Token *t = parser_peek(p);
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
                break; /* while-condition filters to exactly these kinds */
        }
        parser_advance(p);
        t = parser_peek(p);
    }

    /* §6.7.2p2 constraint checks. */
    if (n_signed && n_unsigned)
    {
        parser_error(p, "cannot combine 'signed' and 'unsigned'");
        return NULL;
    }
    if (n_signed > 1 || n_unsigned > 1 || n_int > 1)
    {
        parser_error(p, "duplicate type specifier");
        return NULL;
    }
    if (n_char && (n_short || n_int || n_long))
    {
        parser_error(p, "cannot combine 'char' with short/int/long");
        return NULL;
    }
    if (n_short && n_long)
    {
        parser_error(p, "cannot combine 'short' and 'long'");
        return NULL;
    }
    if (n_long > 2)
    {
        parser_error(p, "too many 'long' type specifiers");
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

/* `_Alignas ( type-name )` / `_Alignas ( constant-expression )` (§6.7.5). The
   folded value must be a valid alignment: a nonzero power of two. Multiple
   `_Alignas` on one declaration combine to the strictest (D14.6: the result is
   recorded on the AST and honored up to the object's natural alignment only). */
static u32 parse_alignas_specifier(ParserCtx *p)
{
    Token *start = parser_peek(p);
    ASSERT(start->kind == TOK_KW_ALIGNAS);
    parser_advance(p);

    if (!parser_expect(p, TOK_LPAREN, "'('"))
    {
        return 0;
    }

    i64 align = 0;
    bool ok;
    /* The `(` is consumed; the type-name/expression token sits at p->pos. */
    if (is_typename_start_at(p, skip_const_ahead(p, p->pos)))
    {
        Type *ty = parse_type_specifier(p);
        if (ty)
        {
            ty = parse_abstract_declarator(p, ty);
            if (ty)
            {
                ty = parse_array_suffix(p, ty);
            }
        }
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
        ok = fold_constant_expr(p, expr, &align);
    }
    if (!ok)
    {
        parser_error(p, "alignment is not a constant expression");
        return 0;
    }
    if (!parser_expect(p, TOK_RPAREN, "')'"))
    {
        return 0;
    }

    if (align <= 0 || (align & (align - 1)) != 0)
    {
        parser_error(p, "invalid alignment (must be a nonzero power of two)");
        return 0;
    }
    return (u32) align;
}

/* Full declaration-specifier list: leading + trailing qualifiers around a
   specifier. Does NOT consume declarator decorators (`*`, `[dims]`) — those
   belong to the per-declarator layer, so `int *a, b;` splits correctly. */
static DeclSpecifiers parse_decl_specifiers(ParserCtx *p)
{
    DeclSpecifiers out = {NULL, NULL, 0};
    Token *t = parser_peek(p);
    Type *ty = NULL;

    /* Leading qualifiers and alignment specifiers: `const int`,
       `_Alignas(16) int`, `const _Alignas(8) long`. */
    bool lead_const = false;
    for (;;)
    {
        if (t->kind == TOK_KW_CONST)
        {
            lead_const = true;
            parser_advance(p);
        }
        else if (t->kind == TOK_KW_ALIGNAS)
        {
            u32 a = parse_alignas_specifier(p);
            if (a == 0)
            {
                return out;
            }
            out.alignas = a > out.alignas ? a : out.alignas;
        }
        else
        {
            break;
        }
        t = parser_peek(p);
    }

    switch (t->kind)
    {
        case TOK_KW_INT:
        case TOK_KW_CHAR:
        case TOK_KW_SHORT:
        case TOK_KW_LONG:
        case TOK_KW_UNSIGNED:
        case TOK_KW_SIGNED:
            ty = parse_integer_specifiers(p);
            break;
        case TOK_KW_BOOL:
            parser_advance(p);
            ty = type_cbool();
            break;
        case TOK_KW_VOID:
            parser_advance(p);
            ty = type_void();
            break;
        case TOK_KW_STRUCT:
        case TOK_KW_UNION:
            ty = parse_record_specifier(p, t->kind == TOK_KW_UNION, &out.tag_def);
            break;
        case TOK_KW_ENUM:
            ty = parse_enum_specifier(p, &out.tag_def);
            break;
        case TOK_IDENT:
        {
            /* A typedef name is a full declaration specifier (D12.3): the
               interned type it aliases is used as-is, then any `*`/qualifier
               suffix below applies. A plain (non-typedef) identifier here is
               the ordinary "expected type specifier" error. */
            ParserBinding *b = name_lookup(p, t->payload.str);
            if (b && b->kind == BIND_TYPEDEF)
            {
                parser_advance(p);
                ty = b->type;
                break;
            }
            parser_error(p, "expected type specifier");
            return out;
        }
        default:
            parser_error(p, "expected type specifier");
            return out;
    }
    if (!ty)
    {
        return out;
    }

    /* Trailing qualifiers and alignment specifiers apply to the type itself:
       `int const x`, `int _Alignas(16) x`, in any order. */
    if (lead_const)
    {
        ty = type_const(ty);
    }
    for (;;)
    {
        Token *nt = parser_peek(p);
        if (nt->kind == TOK_KW_CONST)
        {
            parser_advance(p);
            ty = type_const(ty);
        }
        else if (nt->kind == TOK_KW_ALIGNAS)
        {
            u32 a = parse_alignas_specifier(p);
            if (a == 0)
            {
                return out;
            }
            out.alignas = a > out.alignas ? a : out.alignas;
        }
        else
        {
            break;
        }
    }

    out.type = ty;
    return out;
}

static Type *parse_type_specifier(ParserCtx *p)
{
    return parse_decl_specifiers(p).type;
}

/* The abstract-declarator version of the pointer decorators
   (`{* const}*`): a type-name in cast/sizeof position, which cannot carry a
   name. Array suffixes remain the caller's (casts/sizeof route them through
   parse_array_suffix). */
static Type *parse_abstract_declarator(ParserCtx *p, Type *base)
{
    while (parser_peek(p)->kind == TOK_STAR)
    {
        parser_advance(p);
        base = type_ptr(base);
        while (parser_peek(p)->kind == TOK_KW_CONST)
        {
            parser_advance(p);
            base = type_const(base);
        }
    }
    return base;
}

/* A concrete declarator `{* const}* name [dims]` attached to a specifier's
   base type. `int *a, b;` gives a and b the same base with independent
   decorators (`b` stays `int`). */
static bool parse_declarator(ParserCtx *p, Type *base, Type **out_type, const char **out_name)
{
    *out_type = base;
    *out_name = NULL;

    while (parser_peek(p)->kind == TOK_STAR)
    {
        parser_advance(p);
        *out_type = type_ptr(*out_type);
        while (parser_peek(p)->kind == TOK_KW_CONST)
        {
            parser_advance(p);
            *out_type = type_const(*out_type);
        }
    }

    Token *name = parser_peek(p);
    if (name->kind != TOK_IDENT)
    {
        parser_error(p, "expected declarator name");
        return false;
    }
    parser_advance(p);
    *out_name = name->payload.str;

    Type *with_dims = parse_array_suffix(p, *out_type);
    if (!with_dims)
    {
        return false;
    }
    *out_type = with_dims;
    return true;
}

/* The `name (= init)?` run of an init-declarator list, minus specifiers. */
static bool parse_one_declarator(ParserCtx *p, Type *base, StorageClass storage, u32 alignas,
                                 Vec *decls_out)
{
    Token *decl_start = parser_peek(p);
    Type *dtype;
    const char *name;
    if (!parse_declarator(p, base, &dtype, &name))
    {
        return false;
    }

    if (!parser_check_not_enumerator(p, name))
    {
        return false;
    }
    if (!name_declare(p, name, BIND_VAR, NULL))
    {
        return false;
    }

    ASTNode *decl = ast_var_decl(dtype, name, NULL, storage, decl_start->loc, p->arena);
    ASTVarDecl *vd = ast_as(ASTVarDecl, decl);
    vd->alignas = alignas;
    if (parser_peek(p)->kind == TOK_ASSIGN)
    {
        parser_advance(p);
        ASTNode *expr = parse_initializer(p);
        if (!expr)
        {
            return false;
        }
        if (storage == SC_STATIC)
        {
            if (!resolve_constant_init(p, vd, expr))
            {
                parser_error(p, "initializer for static variable must be a constant "
                                "expression");
                return false;
            }
        }
        else
        {
            vd->init = expr;
        }
    }
    vec_push(decls_out, decl);
    return true;
}

static ASTNode *parse_param(ParserCtx *p)
{
    Token *start = parser_peek(p);
    DeclSpecifiers specs = parse_decl_specifiers(p);
    if (!specs.type)
    {
        return NULL;
    }

    Type *type;
    const char *name;
    if (!parse_declarator(p, specs.type, &type, &name))
    {
        return NULL;
    }

    if (!parser_check_not_enumerator(p, name))
    {
        return NULL;
    }
    if (!name_declare(p, name, BIND_VAR, NULL))
    {
        return NULL;
    }

    /* Array parameters decay to pointer (C11 §6.7.6.3p7); the declarator
            already applied the array suffixes. */
    type = type_decay(type);

    ASTNode *decl = ast_var_decl(type, name, NULL, SC_NONE, start->loc, p->arena);
    ast_as(ASTVarDecl, decl)->alignas = specs.alignas;
    return decl;
}

static Vec *parse_param_list(ParserCtx *p, bool *out_is_variadic)
{
    if (out_is_variadic)
    {
        *out_is_variadic = false;
    }
    Vec *params = vec_new(p->arena);
    Token *t = parser_peek(p);

    /* `(void)` is the explicit empty-parameter-list marker (C11 §6.7.6.3p10).
       `void` followed by anything else (`void *p`, `void *const p`) is an
       ordinary parameter whose type (pointer to void) is built by
       parse_param; the lexer never splits a `void *` across tokens. */
    if (t->kind == TOK_KW_VOID && p->pos + 1 < p->count && p->tokens[p->pos + 1].kind == TOK_RPAREN)
    {
        parser_advance(p);
        return params;
    }

    if (t->kind == TOK_RPAREN)
    {
        return params;
    }

    /* `...` must follow at least one named parameter (C11 §6.7.6.3p8). */
    if (t->kind == TOK_ELLIPSIS)
    {
        parser_error(p, "'...' must follow at least one named parameter");
        return NULL;
    }

    ASTNode *first = parse_param(p);
    if (!first)
    {
        return NULL;
    }
    vec_push(params, first);

    while (parser_peek(p)->kind == TOK_COMMA)
    {
        parser_advance(p);
        if (parser_peek(p)->kind == TOK_ELLIPSIS)
        {
            /* `...` must be the final element of the parameter list. */
            if (!(p->pos + 1 < p->count && p->tokens[p->pos + 1].kind == TOK_RPAREN))
            {
                parser_error(p, "expected ')' after '...'");
                return NULL;
            }
            parser_advance(p);
            if (out_is_variadic)
            {
                *out_is_variadic = true;
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

static ASTNode *parse_compound_stmt(ParserCtx *p)
{
    Token *start = parser_peek(p);
    if (!parser_expect(p, TOK_LBRACE, "'{'"))
    {
        return NULL;
    }
    push_name_scope(p);

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
    pop_name_scope(p);

    return ast_compound_stmt(stmts, start->loc, p->arena);
}

static ASTNode *parse_return_stmt(ParserCtx *p)
{
    Token *start = parser_peek(p);
    ASSERT(start->kind == TOK_KW_RETURN);
    parser_advance(p);

    ASTNode *expr = NULL;
    if (parser_peek(p)->kind != TOK_SEMI)
    {
        expr = parse_expression(p);
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
    /* C11 §6.7.6.2: `int a[2][3]` is array[2] of array[3] of int — the leftmost
       bracket is the outermost dimension. Collect the lengths first so they
       nest rightmost-innermost (array suffixes apply left-to-right). */
    u64 dims[32];
    size_t ndim = 0;
    while (parser_peek(p)->kind == TOK_LBRACKET)
    {
        parser_advance(p);
        u64 len = 0;
        if (parser_peek(p)->kind != TOK_RBRACKET)
        {
            ASTNode *size_expr = parse_assign(p);
            if (!size_expr || size_expr->kind != AST_INT_LITERAL)
            {
                parser_error(p, "array size must be an integer constant");
                return NULL;
            }
            len = (u64) ast_as(ASTIntLiteral, size_expr)->value;
        }
        parser_expect(p, TOK_RBRACKET, "]");
        if (ndim == 32)
        {
            parser_error(p, "too many array dimensions");
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

static ASTNode *parse_typedef_decl(ParserCtx *p)
{
    Token *start = parser_peek(p);
    ASSERT(start->kind == TOK_KW_TYPEDEF);
    parser_advance(p);

    DeclSpecifiers specs = parse_decl_specifiers(p);
    if (!specs.type)
    {
        return NULL;
    }

    /* §6.7.5p3: an alignment specifier shall not be used in a typedef. */
    if (specs.alignas)
    {
        parser_error(p, "_Alignas is not permitted in a typedef");
        return NULL;
    }

    Type *dtype;
    const char *name;
    if (!parse_declarator(p, specs.type, &dtype, &name))
    {
        return NULL;
    }

    if (!parser_check_not_enumerator(p, name))
    {
        return NULL;
    }
    if (!name_declare(p, name, BIND_TYPEDEF, dtype))
    {
        return NULL;
    }

    if (parser_peek(p)->kind != TOK_SEMI)
    {
        parser_error(p, "expected ';' after typedef declaration");
        return NULL;
    }
    parser_advance(p);

    return ast_typedef_decl(dtype, name, start->loc, p->arena);
}

/* A block-scope declaration: specifiers plus an init-declarator list
   (§6.7.6). A single declarator returns a bare AST_VAR_DECL; two or more
   wrap in AST_DECL_LIST. With zero declarators the declaration must be a
   tagged struct/union/enum *definition* (`struct S { ... };`) and returns
   that AST node (downstream treats it as a no-op — the type is complete at
   parse time). */
static ASTNode *parse_var_decl(ParserCtx *p, StorageClass storage)
{
    Token *start = parser_peek(p);
    DeclSpecifiers specs = parse_decl_specifiers(p);
    if (!specs.type)
    {
        return NULL;
    }

    if (parser_peek(p)->kind == TOK_SEMI)
    {
        parser_advance(p);
        if (specs.tag_def)
        {
            return specs.tag_def;
        }
        parser_error(p, "declaration declares nothing");
        return NULL;
    }

    Vec *decls = vec_new(p->arena);
    while (true)
    {
        if (!parse_one_declarator(p, specs.type, storage, specs.alignas, decls))
        {
            return NULL;
        }
        if (parser_peek(p)->kind != TOK_COMMA)
        {
            break;
        }
        parser_advance(p);
    }

    if (!parser_expect(p, TOK_SEMI, "';'"))
    {
        return NULL;
    }

    if (vec_size(decls) == 1)
    {
        return (ASTNode *) vec_get(decls, 0);
    }
    return ast_decl_list(decls, start->loc, p->arena);
}

static ASTNode *parse_expr_stmt(ParserCtx *p)
{
    Token *start = parser_peek(p);
    ASTNode *expr = parse_expression(p);
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
    parser_advance(p);

    if (!parser_expect(p, TOK_LPAREN, "'('"))
    {
        return NULL;
    }

    ASTNode *cond = parse_expression(p);
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
        parser_advance(p);
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
    parser_advance(p);

    if (!parser_expect(p, TOK_LPAREN, "'('"))
    {
        return NULL;
    }

    ASTNode *cond = parse_expression(p);
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
    parser_advance(p);

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

    ASTNode *cond = parse_expression(p);
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
    parser_advance(p);

    if (!parser_expect(p, TOK_LPAREN, "'('"))
    {
        return NULL;
    }

    ASTNode *init = NULL;
    if (parser_peek(p)->kind != TOK_SEMI)
    {
        /* A for-init may open a declaration with a type keyword, a visible
           typedef name, or leading `const` (D12.3); anything else is an
           expression statement. */
        if (is_typename_start(p) || parser_peek(p)->kind == TOK_KW_CONST)
        {
            init = parse_var_decl(p, SC_NONE);
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
        parser_advance(p);
    }

    ASTNode *cond = NULL;
    if (parser_peek(p)->kind != TOK_SEMI)
    {
        cond = parse_expression(p);
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
        post = parse_expression(p);
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
    parser_advance(p);
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
    parser_advance(p);
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
    parser_advance(p);

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
    parser_advance(p);

    ASTNode *stmt = parse_stmt(p);
    if (!stmt)
    {
        return NULL;
    }

    return ast_label_stmt(label, stmt, start->loc, p->arena);
}

static ASTNode *parse_switch_stmt(ParserCtx *p)
{
    Token *start = parser_peek(p);
    ASSERT(start->kind == TOK_KW_SWITCH);
    parser_advance(p);

    if (!parser_expect(p, TOK_LPAREN, "'('"))
    {
        return NULL;
    }

    ASTNode *cond = parse_expression(p);
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

    return ast_switch_stmt(cond, body, start->loc, p->arena);
}

static ASTNode *parse_case_stmt(ParserCtx *p)
{
    Token *start = parser_peek(p);
    ASSERT(start->kind == TOK_KW_CASE);
    parser_advance(p);

    ASTNode *expr = parse_assign(p);
    if (!expr)
    {
        return NULL;
    }

    i64 value = 0;
    /* If the constant expression can't be folded yet (e.g. `case sizeof(x):`
       where the type of x is only resolved by semantic), defer to semantic,
       which has the types to evaluate it. */
    bool value_known = fold_constant_expr(p, expr, &value);

    if (!parser_expect(p, TOK_COLON, "':'"))
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

static ASTNode *parse_default_stmt(ParserCtx *p)
{
    Token *start = parser_peek(p);
    ASSERT(start->kind == TOK_KW_DEFAULT);
    parser_advance(p);

    if (!parser_expect(p, TOK_COLON, "':'"))
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

/* `_Static_assert( constant-expression , string-literal ) ;` (§6.7.4). The
   constant expression is a full *constant-expression* (comma is the
   separator, not the comma operator, so the argument parses at the assignment
   level). The message must be a string literal. Folding and checking happen in
   the semantic pass (types like `sizeof(x)` are unknown here); this function
   only builds the node. */
static ASTNode *parse_static_assert(ParserCtx *p)
{
    Token *start = parser_peek(p);
    ASSERT(start->kind == TOK_KW_STATIC_ASSERT);
    parser_advance(p);

    if (!parser_expect(p, TOK_LPAREN, "'('"))
    {
        return NULL;
    }

    ASTNode *expr = parse_assign(p);
    if (!expr)
    {
        return NULL;
    }

    if (!parser_expect(p, TOK_COMMA, "','"))
    {
        return NULL;
    }

    Token *msg = parser_peek(p);
    if (msg->kind != TOK_STRING_LIT)
    {
        parser_error(p, "expected string literal in _Static_assert");
        return NULL;
    }
    parser_advance(p);

    if (!parser_expect(p, TOK_RPAREN, "')'"))
    {
        return NULL;
    }
    if (!parser_expect(p, TOK_SEMI, "';'"))
    {
        return NULL;
    }

    return ast_static_assert(expr, msg->payload.str, start->loc, p->arena);
}

/* In C, a `case N:` label applies to every statement that follows it until
   the next case/default label. */

static ASTNode *parse_stmt(ParserCtx *p)
{
    Token *t = parser_peek(p);
    switch (t->kind)
    {
        case TOK_KW_INT:
        case TOK_KW_BOOL:
        case TOK_KW_CHAR:
        case TOK_KW_SHORT:
        case TOK_KW_LONG:
        case TOK_KW_UNSIGNED:
        case TOK_KW_SIGNED:
        case TOK_KW_VOID:
        case TOK_KW_STRUCT:
        case TOK_KW_UNION:
        case TOK_KW_ENUM:
            return parse_var_decl(p, SC_NONE);
        case TOK_KW_TYPEDEF:
            return parse_typedef_decl(p);
        case TOK_KW_STATIC:
            parser_advance(p);
            return parse_var_decl(p, SC_STATIC);
        case TOK_KW_EXTERN:
            parser_advance(p);
            return parse_var_decl(p, SC_EXTERN);
        case TOK_KW_CONST:
        {
            /* const may either precede the storage class (`const static int x`)
               or the type (`const int x`); the latter is consumed by
               parse_decl_specifiers. */
            size_t nconst = 0;
            while (p->pos + nconst < p->count && p->tokens[p->pos + nconst].kind == TOK_KW_CONST)
            {
                nconst++;
            }
            TokenKind nxt =
                (p->pos + nconst < p->count) ? p->tokens[p->pos + nconst].kind : TOK_EOF;
            if (nxt == TOK_KW_STATIC || nxt == TOK_KW_EXTERN)
            {
                for (size_t i = 0; i < nconst; i++)
                {
                    parser_advance(p);
                }
                parser_advance(p);
                return parse_var_decl(p, nxt == TOK_KW_STATIC ? SC_STATIC : SC_EXTERN);
            }
            return parse_var_decl(p, SC_NONE);
        }
        case TOK_KW_ALIGNAS:
            /* `_Alignas(16) int x;` at block scope (an alignment specifier is
               a declaration specifier; storage class before it is handled by
               the above storage-class cases). */
            return parse_var_decl(p, SC_NONE);
        case TOK_KW_STATIC_ASSERT:
            return parse_static_assert(p);
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
        case TOK_KW_SWITCH:
            return parse_switch_stmt(p);
        case TOK_KW_CASE:
            return parse_case_stmt(p);
        case TOK_KW_DEFAULT:
            return parse_default_stmt(p);
        case TOK_LBRACE:
            return parse_compound_stmt(p);
        case TOK_IDENT:
            if (p->pos + 1 < p->count && p->tokens[p->pos + 1].kind == TOK_COLON)
            {
                return parse_label_stmt(p);
            }
            if (is_typename_start(p))
            {
                return parse_var_decl(p, SC_NONE);
            }
            return parse_expr_stmt(p);
        default:
            return parse_expr_stmt(p);
    }
}

/* The C11 *expression* level (§6.5.17): comma-separated, left-associative,
   value of the rightmost operand. This is DIFFERENT from parse_assign (the
   §6.5.16 *assignment-expression* level) — commas in argument lists,
   init-list elements, designator indexes, array sizes, and `case` labels are
   separators, not the operator, so those sites call parse_assign. Only C11's
   *expression* positions (expression statements, `return`, conditions,
   for-clauses, subscript indexes, ternary middle, parenthesized primaries)
   come through the comma-aware parse_expression. */
static ASTNode *parse_expression(ParserCtx *p)
{
    ASTNode *left = parse_assign(p);
    if (!left)
    {
        return NULL;
    }
    while (parser_peek(p)->kind == TOK_COMMA)
    {
        Token *t = parser_peek(p);
        parser_advance(p);
        ASTNode *right = parse_assign(p);
        if (!right)
        {
            return NULL;
        }
        left = ast_binary_expr(BIN_COMMA, left, right, t->loc, p->arena);
    }
    return left;
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
                ASTNode *arg = parse_assign(p);
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

/* `__builtin_va_arg ( assignment-expression , type-name )` (D15.3): the second
   argument is a type-name, so this cannot ride through the ordinary call path.
   The assignment-expression level stops at the comma, and the type-name uses
   the same specifier + abstract-declarator machinery as casts. */
static ASTNode *parse_builtin_va_arg(ParserCtx *p, Token *t)
{
    parser_advance(p); /* consume `__builtin_va_arg` */
    if (!parser_expect(p, TOK_LPAREN, "'('"))
    {
        return NULL;
    }
    ASTNode *ap = parse_assign(p);
    if (!ap)
    {
        return NULL;
    }
    if (!parser_expect(p, TOK_COMMA, "','"))
    {
        return NULL;
    }
    if (!is_typename_start_at(p, p->pos))
    {
        parser_error(p, "expected a type name after ',' in '__builtin_va_arg'");
        return NULL;
    }
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
    if (!parser_expect(p, TOK_RPAREN, "')'"))
    {
        return NULL;
    }
    return ast_va_arg_expr(ap, ty, t->loc, p->arena);
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
        case TOK_CHAR_LIT:
        {
            /* A character constant has type int (§6.4.4.4p10), so it is
               exactly an integer literal — the whole downstream pipeline
               (folding, `case 'a':`, initializers) works with no extra code. */
            parser_advance(p);
            return ast_int_literal(t->payload.int_val, false, SUFFIX_NONE, false, t->loc, p->arena);
        }
        case TOK_IDENT:
        {
            if (strcmp(t->payload.str, "__builtin_va_arg") == 0 && p->pos + 1 < p->count &&
                p->tokens[p->pos + 1].kind == TOK_LPAREN)
            {
                return parse_builtin_va_arg(p, t);
            }
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
            /* A parenthesized expression is a primary expression (§6.5.1p5)
               — it takes the full expression level, so `(a, b)` works. */
            ASTNode *inner = parse_expression(p);
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
    if (t->kind == TOK_LPAREN)
    {
        /* Cast: `(type-name) unary`. Disambiguate from a parenthesized
           expression by the token stream after `(` (skipping leading `const`):
           a cast opens with a type-specifier keyword or a visible typedef
           identifier (D12.3 — retires the Phase 11 "exact because no typedefs
           exist" rule). Everything else (`ident`, `ident +`, `(`, number…) is
           a parenthesized expression. */
        size_t look = skip_const_ahead(p, p->pos + 1);
        if (is_typename_start_at(p, look))
        {
            parser_advance(p); /* consume '(' */
            Type *target = parse_type_specifier(p);
            if (!target)
            {
                return NULL;
            }
            target = parse_abstract_declarator(p, target);
            if (!target)
            {
                return NULL;
            }
            if (parser_peek(p)->kind == TOK_LBRACKET)
            {
                /* The type name carries an array suffix (`(int[3])`,
                   `(int[])`) — that is never a cast target, so this must be a
                   compound literal. The suffix lives inside the parens. */
                target = parse_array_suffix(p, target);
                if (!target)
                {
                    return NULL;
                }
                if (!parser_expect(p, TOK_RPAREN, "')'"))
                {
                    return NULL;
                }
                if (parser_peek(p)->kind != TOK_LBRACE)
                {
                    parser_error(p, "expected '{' after compound literal type name");
                    return NULL;
                }
                ASTNode *init = parse_init_list(p);
                if (!init)
                {
                    return NULL;
                }
                return parse_postfix_ops(p, ast_compound_literal(target, init, t->loc, p->arena));
            }
            if (!parser_expect(p, TOK_RPAREN, "')'"))
            {
                return NULL;
            }
            if (parser_peek(p)->kind == TOK_LBRACE)
            {
                /* Compound literal (D12.9): `(type) { ... }`. The `{` after
                   `)` is the unambiguous discriminator — `{` is never a unary
                   operand, so this cannot collide with a cast. */
                ASTNode *init = parse_init_list(p);
                if (!init)
                {
                    return NULL;
                }
                ASTNode *cl = ast_compound_literal(target, init, t->loc, p->arena);
                /* Postfix ops bind tighter than the compound literal's brace
                   list closes: `(struct S){...}.x`. */
                return parse_postfix_ops(p, cl);
            }
            ASTNode *operand = parse_unary(p);
            if (!operand)
            {
                return NULL;
            }
            return ast_cast_expr(target, operand, t->loc, p->arena);
        }
    }
    static const struct
    {
        TokenKind tok;
        UnaryOpKind uop;
    } unary_ops[] = {
        {TOK_MINUS, UN_NEG},  {TOK_NOT, UN_LOG_NOT}, {TOK_TILDE, UN_BIT_NOT},
        {TOK_STAR, UN_DEREF}, {TOK_BW_AND, UN_ADDR},
    };
    if (t->kind == TOK_PLUS_PLUS || t->kind == TOK_MINUS_MINUS)
    {
        /* Prefix `++`/`--`: the operand is a unary-expression, so `++++x`
           parses `++(++x)` (semantic rejects the inner rvalue). */
        bool is_inc = t->kind == TOK_PLUS_PLUS;
        parser_advance(p);
        ASTNode *operand = parse_unary(p);
        if (!operand)
        {
            return NULL;
        }
        return ast_incdec_expr(operand, is_inc, false, t->loc, p->arena);
    }
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
            is_type = is_typename_start_at(p, skip_const_ahead(p, p->pos + 1));
        }
        if (is_type)
        {
            parser_advance(p);
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
    if (t->kind == TOK_KW_ALIGNOF)
    {
        parser_advance(p);
        /* C11 §6.5.3.4 takes a type-name; the expression form is a ficc/gcc
           extension (D14.4). Both are integer constant expressions and fold
           to the operand's alignment. */
        if (parser_peek(p)->kind == TOK_LPAREN)
        {
            if (is_typename_start_at(p, skip_const_ahead(p, p->pos + 1)))
            {
                parser_advance(p);
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
                if (!parser_expect(p, TOK_RPAREN, ")"))
                {
                    return NULL;
                }
                return ast_alignof_type(ty, 0, t->loc, p->arena);
            }
        }
        ASTNode *operand = parse_unary(p);
        if (!operand)
        {
            return NULL;
        }
        return ast_alignof_expr(operand, 0, t->loc, p->arena);
    }
    return parse_postfix(p);
}

/* Postfix operator loop over an already-parsed operand: `[i]` subscript and
   `.field`/`->field` member access. Shared by parse_postfix and the compound
   literal branch of parse_unary (a compound literal is a postfix expression,
   so `(T){...}.x` / `(T){...}[i]` keep chaining, C11 §6.5.2.5). */
static ASTNode *parse_postfix_ops(ParserCtx *p, ASTNode *node)
{
    while (true)
    {
        Token *t = parser_peek(p);
        if (t->kind == TOK_LBRACKET)
        {
            parser_advance(p);
            /* Subscript takes a full expression (§6.5.2.1), so the comma
               operator is legal in the index: `a[i, j]`. */
            ASTNode *index = parse_expression(p);
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
        else if (t->kind == TOK_PLUS_PLUS || t->kind == TOK_MINUS_MINUS)
        {
            /* Postfix `++`/`--`: binds tighter than anything in parse_unary
               (`*p++` = `*(p++)`); continue chaining so `p++->x`, `a[i++]`
               keep nesting. */
            parser_advance(p);
            node = ast_incdec_expr(node, t->kind == TOK_PLUS_PLUS, true, t->loc, p->arena);
        }
        else
        {
            break;
        }
    }
    return node;
}

static ASTNode *parse_postfix(ParserCtx *p)
{
    ASTNode *node = parse_primary(p);
    if (!node)
    {
        return NULL;
    }
    return parse_postfix_ops(p, node);
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
    parser_advance(p);
    /* The middle operand of `?:` is a full expression (§6.5.15), so commas
       are legal there: `c ? (a, b) : d` and even `c ? a, b : d`. */
    ASTNode *then_expr = parse_expression(p);
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

static ASTNode *parse_assign(ParserCtx *p)
{
    ASTNode *left = parse_ternary(p);
    if (!left)
    {
        return NULL;
    }

    BinOpKind op = assignment_op(parser_peek(p)->kind);
    if (op != (BinOpKind) -1)
    {
        Token *t = parser_peek(p);
        parser_advance(p);
        ASTNode *right = parse_assign(p);
        if (!right)
        {
            return NULL;
        }
        left = ast_binary_expr(op, left, right, t->loc, p->arena);
    }
    return left;
}

/* A designator chain (C11 §6.7.9p1): `.field` members and `[index]` array
   elements, outermost first. GNU `[a ... b]` ranges are rejected. Returns
   NULL when the next token opens none. */
static Designator *parse_designators(ParserCtx *p)
{
    Designator *head = NULL;
    Designator **tail = &head;
    while (parser_peek(p)->kind == TOK_DOT || parser_peek(p)->kind == TOK_LBRACKET)
    {
        Designator *d = arena_alloc(p->arena, sizeof(Designator), _Alignof(Designator));
        d->next = NULL;
        if (parser_peek(p)->kind == TOK_DOT)
        {
            parser_advance(p);
            Token *name = parser_peek(p);
            if (name->kind != TOK_IDENT)
            {
                parser_error(p, "expected field name after '.' designator");
                return NULL;
            }
            parser_advance(p);
            d->kind = ND_FIELD;
            d->field = name->payload.str;
            d->index = 0;
        }
        else
        {
            parser_advance(p);
            ASTNode *idx = parse_assign(p);
            if (!idx || idx->kind != AST_INT_LITERAL)
            {
                parser_error(p, "array designator index must be an integer constant");
                return NULL;
            }
            if (!parser_expect(p, TOK_RBRACKET, "']'"))
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

/* A brace-enclosed initializer list `{ elem1, elem2, ... }` with optional
   designators and a trailing comma allowed. */
static ASTNode *parse_init_list(ParserCtx *p)
{
    Token *start = parser_peek(p);
    ASSERT(start->kind == TOK_LBRACE);
    parser_advance(p);

    Vec *elems = vec_new(p->arena);
    while (parser_peek(p)->kind != TOK_RBRACE)
    {
        Token *elem_start = parser_peek(p);
        InitElem *e = arena_alloc(p->arena, sizeof(InitElem), _Alignof(InitElem));
        e->loc = elem_start->loc;
        e->design = parse_designators(p);
        if (!e->design)
        {
            e->value = parse_initializer(p);
        }
        else
        {
            if (!parser_expect(p, TOK_ASSIGN, "'='"))
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

        TokenKind sep = parser_peek(p)->kind;
        if (sep == TOK_COMMA)
        {
            parser_advance(p);
            if (parser_peek(p)->kind == TOK_RBRACE)
            {
                break; /* trailing comma */
            }
        }
        else if (sep != TOK_RBRACE)
        {
            parser_error(p, "expected ',' or '}' in initializer list");
            return NULL;
        }
    }
    if (!parser_expect(p, TOK_RBRACE, "'}'"))
    {
        return NULL;
    }

    return ast_init_list(elems, start->loc, p->arena);
}

/* The right-hand side of an initializer: a brace-enclosed list, or a plain
   assignment-expression (C11 §6.7.9p2). */
static ASTNode *parse_initializer(ParserCtx *p)
{
    if (parser_peek(p)->kind == TOK_LBRACE)
    {
        return parse_init_list(p);
    }
    return parse_assign(p);
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
                        return false;
                    }
                    *out = b->op == BIN_DIV ? l / r : l % r;
                    return true;
                case BIN_SHL:
                case BIN_SHR:
                    if (r < 0 || r > 63)
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
            if (!fold_constant_expr(p, te->cond, &cond))
            {
                return false;
            }
            if (cond)
            {
                return fold_constant_expr(p, te->then_expr, out);
            }
            return fold_constant_expr(p, te->else_expr, out);
        }
        case AST_SIZEOF_TYPE:
        {
            /* sizeof(type) is an integer constant expression (§6.6p6). */
            ASTSizeofType *st = ast_as(ASTSizeofType, node);
            *out = (i64) type_sizeof(st->type);
            return true;
        }
        case AST_ALIGNOF_TYPE:
        {
            /* _Alignof(type) is an integer constant expression (§6.6p6);
               the type is complete at parse time, so fold here. */
            ASTAlignofType *at = ast_as(ASTAlignofType, node);
            *out = (i64) type_alignof(at->type);
            return true;
        }
        case AST_CAST_EXPR:
        {
            /* Casts are legal operators inside an integer constant expression
               (§6.6p3/p6): fold the operand, then convert it into the target
               type's range. Only integer targets fold (a pointer cast is an
               address constant, not an integer constant). */
            ASTCastExpr *ce = ast_as(ASTCastExpr, node);
            i64 v;
            if (!fold_constant_expr(p, ce->operand, &v) || !type_is_integer(ce->target_type))
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

/* Fold a file-scope/static initializer: a constant expression is stored as
   const_init; a string literal (char * target) or address constant (`&g`)
   survives as init for the .data relocation path; a brace-enclosed list is
   routed to semantic's planner and the IR serializer (D12.6), so it survives
   as init too. */
static bool resolve_constant_init(ParserCtx *p, ASTVarDecl *vd, ASTNode *expr)
{
    if (expr->kind == AST_INIT_LIST)
    {
        vd->init = expr;
        return true;
    }
    if (expr->kind == AST_UNARY_EXPR)
    {
        /* Address constant (§6.6p9): `&g` or `&(type){...}` (a compound
           literal is an address constant; D12.9). The *target* is checked in
           semantic (it must be an object of static storage duration), and the
           relocation is serialized in the IR builder. */
        ASTUnaryExpr *u = ast_as(ASTUnaryExpr, expr);
        if (u->op == UN_ADDR &&
            (u->operand->kind == AST_IDENT || u->operand->kind == AST_COMPOUND_LITERAL))
        {
            vd->init = expr;
            return true;
        }
    }
    i64 value;
    if (fold_constant_expr(p, expr, &value))
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

static ASTNode *parse_top_level_decl(ParserCtx *p)
{
    Token *start = parser_peek(p);

    if (parser_peek(p)->kind == TOK_KW_TYPEDEF)
    {
        return parse_typedef_decl(p);
    }

    if (parser_peek(p)->kind == TOK_KW_STATIC_ASSERT)
    {
        return parse_static_assert(p);
    }

    StorageClass storage = SC_NONE;
    u32 pre_storage_consts = 0; /* consts consumed before static/extern; the
                                   type must still be qualified by them */
    if (parser_peek(p)->kind == TOK_KW_STATIC || parser_peek(p)->kind == TOK_KW_EXTERN)
    {
        storage = parser_peek(p)->kind == TOK_KW_STATIC ? SC_STATIC : SC_EXTERN;
        parser_advance(p);
    }
    else if (parser_peek(p)->kind == TOK_KW_CONST)
    {
        /* A qualifier may precede the storage class: `const static int g;`. */
        size_t nconst = 0;
        while (p->pos + nconst < p->count && p->tokens[p->pos + nconst].kind == TOK_KW_CONST)
        {
            nconst++;
        }
        TokenKind nxt = (p->pos + nconst < p->count) ? p->tokens[p->pos + nconst].kind : TOK_EOF;
        if (nxt == TOK_KW_STATIC || nxt == TOK_KW_EXTERN)
        {
            for (size_t i = 0; i < nconst; i++)
            {
                parser_advance(p);
            }
            parser_advance(p);
            storage = nxt == TOK_KW_STATIC ? SC_STATIC : SC_EXTERN;
            pre_storage_consts = (u32) nconst;
        }
    }

    DeclSpecifiers specs = parse_decl_specifiers(p);
    if (!specs.type)
    {
        return NULL;
    }
    for (u32 i = 0; i < pre_storage_consts; i++)
    {
        specs.type = type_const(specs.type);
    }

    /* Bare tagged definition: `struct S { ... };` / `enum E { ... };`. The
       specifier consumed and completed the type; the tag-def AST node is
       returned so the translation unit records it (downstream no-op). */
    if (parser_peek(p)->kind == TOK_SEMI)
    {
        parser_advance(p);
        if (specs.tag_def)
        {
            return specs.tag_def;
        }
        parser_error(p, "declaration declares nothing");
        return NULL;
    }

    Type *dtype;
    const char *name;
    if (!parse_declarator(p, specs.type, &dtype, &name))
    {
        return NULL;
    }

    if (parser_peek(p)->kind == TOK_LPAREN)
    {
        /* §6.7.5p2: an alignment specifier applies to objects and members
           only — never to a function. */
        if (specs.alignas)
        {
            parser_error(p, "_Alignas is not permitted on a function");
            return NULL;
        }
        /* extern on a function definition is an ordinary definition (C11
           §6.9.1); a `;` after the parameter list is a prototype (forward
           declaration, §6.7.6.3) — accepted for the first time in Phase 16. */
        StorageClass fn_storage = storage == SC_STATIC ? SC_STATIC : SC_NONE;
        if (!parser_check_not_enumerator(p, name))
        {
            return NULL;
        }
        if (!name_declare(p, name, BIND_FUNC, NULL))
        {
            return NULL;
        }

        parser_advance(p);
        /* Parameters live in the function's own scope, pushed here and popped
           after the body — mirroring semantic's structure. The compound
           statement pushes/pops its own nested scope. */
        push_name_scope(p);
        bool is_variadic = false;
        Vec *params = parse_param_list(p, &is_variadic);
        if (!params)
        {
            return NULL;
        }

        if (!parser_expect(p, TOK_RPAREN, "')'"))
        {
            return NULL;
        }

        if (parser_peek(p)->kind == TOK_SEMI)
        {
            /* Prototype: no body. */
            parser_advance(p);
            pop_name_scope(p);
            return ast_func_decl(dtype, name, params, fn_storage, is_variadic, start->loc,
                                 p->arena);
        }

        ASTNode *body = parse_compound_stmt(p);
        if (!body)
        {
            return NULL;
        }
        pop_name_scope(p);

        return ast_func_def(dtype, name, params, body, fn_storage, is_variadic, start->loc,
                            p->arena);
    }

    /* File-scope objects: an init-declarator list (multi-declarators share
       the specifier; each declarator's decorators apply independently). */
    Vec *decls = vec_new(p->arena);
    while (true)
    {
        if (!parser_check_not_enumerator(p, name))
        {
            return NULL;
        }
        /* File-scope variables: register the ordinary name. Same-kind repeats
           (extern/static/tentative merging) pass through to semantic (D12.2). */
        if (!name_declare(p, name, BIND_VAR, NULL))
        {
            return NULL;
        }

        ASTNode *decl = ast_var_decl(dtype, name, NULL, storage, start->loc, p->arena);
        ASTVarDecl *vd = ast_as(ASTVarDecl, decl);
        vd->alignas = specs.alignas;
        if (parser_peek(p)->kind == TOK_ASSIGN)
        {
            parser_advance(p);
            ASTNode *expr = parse_initializer(p);
            if (!expr)
            {
                return NULL;
            }
            if (!resolve_constant_init(p, vd, expr))
            {
                parser_error(p, "initializer for file-scope variable must be a constant "
                                "expression");
                return NULL;
            }
        }
        vec_push(decls, decl);

        if (parser_peek(p)->kind != TOK_COMMA)
        {
            break;
        }
        parser_advance(p);
        if (!parse_declarator(p, specs.type, &dtype, &name))
        {
            return NULL;
        }
        if (parser_peek(p)->kind == TOK_LPAREN)
        {
            parser_error(p, "a function definition must be the only declarator in its "
                            "declaration");
            return NULL;
        }
    }

    if (!parser_expect(p, TOK_SEMI, "';'"))
    {
        return NULL;
    }

    if (vec_size(decls) == 1)
    {
        return (ASTNode *) vec_get(decls, 0);
    }
    return ast_decl_list(decls, start->loc, p->arena);
}

ASTNode *parse(Token *tokens, u64 count, Arena *arena)
{
    ASSERT(count > 0);
    ParserCtx p = {tokens, count, 0, arena, strmap_new(arena), vec_new(arena)};
    push_name_scope(&p);

    /* Builtin typedef: `__builtin_va_list` is ficc's own type (D15.2), so the
       __builtin_va_* compiler builtins work without any include. The raw
       `va_list`/`va_start` names are NOT reserved in Phase 15 — they arrive as
       ordinary names via the ficc <stdarg.h> shim + preprocessor in Phase 17.
       A user redefinition to the same type passes the typedef check; a
       redeclaration to a different type is an error (as in gcc). */
    if (!name_declare(&p, "__builtin_va_list", BIND_TYPEDEF, type_va_list()))
    {
        return NULL;
    }

    Vec *decls = vec_new(arena);
    while (parser_peek(&p)->kind != TOK_EOF)
    {
        /* Every translation-unit item — functions, file-scope variables,
           typedefs, and bare/full struct/union/enum *definitions* (the
           specifier consumes and completes inline `{ ... }`) — routes
           through the one declaration parser. */
        ASTNode *node = parse_top_level_decl(&p);
        if (!node)
        {
            return NULL;
        }
        vec_push(decls, node);
    }

    return ast_program(decls, tokens[0].loc, arena);
}
