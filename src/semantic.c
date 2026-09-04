#include "semantic.h"
#include "util/hashmap.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct SemanticCtx SemanticCtx;
struct SemanticCtx
{
    Arena *arena;
    StrMap *globals;     /* function name -> ASTFuncDef */
    StrMap *global_vars; /* file-scope variable name -> ASTVarDecl */
    Vec *scopes;         /* Vec<StrMap*> — lexical scope stack (name -> ASTVarDecl) */
    StrMap *labels;      /* label name -> ASTLabelStmt (collected per function) */
    int loop_depth;
    int switch_depth;
    Vec *switch_sem_stack; /* Vec<SwitchSem*> — per-switch case-value sets */
    bool error;
};

typedef struct SwitchSem SwitchSem;
struct SwitchSem
{
    U64Map *values;      /* (u64)converted case value -> non-NULL, for duplicate detection */
    Type *promoted_cond; /* type_promote(controlling expression type) */
    bool has_default;
};

static Type *check_expr(ASTNode *node, SemanticCtx *ctx);
static bool check_stmt(ASTNode *node, SemanticCtx *ctx, Type *ret_type);
static bool check_func(ASTNode *node, SemanticCtx *ctx);
static bool check_ternary_expression(ASTTernaryExpr *ternary, SemanticCtx *ctx);
static bool check_cast_expr(ASTCastExpr *ce, SemanticCtx *ctx);
static bool plan_list(SemanticCtx *ctx, InitPlan *plan, Type *t, ASTInitList *list, u32 base_off);
static bool plan_char_array_from_string(SemanticCtx *ctx, ASTVarDecl *vd);
static bool plan_ptr_initializer(SemanticCtx *ctx, ASTVarDecl *vd);
static InitPlan *init_plan_new(SemanticCtx *ctx, Type *obj_type);

static void sem_error(Loc loc, const char *fmt, ...)
{
    fprintf(stderr, "%s:%u:%u: [semantic] error: ", loc.file, loc.line, loc.col);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

/* C11 §6.2.5p21: scalar types are arithmetic and pointer types. Records,
   arrays, and void are not scalar. */
static bool is_scalar_type(Type *t)
{
    t = type_unqual(t);
    return t->kind != TYPE_VOID && !type_is_record(t) && !type_is_array(t);
}

/* A value expression that turned out void (`(void)x`, a void function call, a
   dereference of a void object) cannot be used where a value is needed;
   report the classic diagnostic instead of lowering a width-0 vreg. */
static bool check_value_used(ASTNode *node, SemanticCtx *ctx)
{
    if (node->expr_type && node->expr_type->kind == TYPE_VOID)
    {
        sem_error(node->loc, "void value not ignored as it ought to be");
        ctx->error = true;
        return false;
    }
    return true;
}

static StrMap *current_scope(SemanticCtx *ctx)
{
    return (StrMap *) vec_last(ctx->scopes);
}

static void push_scope(SemanticCtx *ctx)
{
    vec_push(ctx->scopes, strmap_new(ctx->arena));
}

static void pop_scope(SemanticCtx *ctx)
{
    vec_pop(ctx->scopes);
}

/* Walk the scope stack innermost-first. */
static ASTVarDecl *scope_lookup(SemanticCtx *ctx, const char *name)
{
    size_t n = vec_size(ctx->scopes);
    for (size_t i = n; i > 0; i--)
    {
        ASTVarDecl *decl = strmap_get((StrMap *) vec_get(ctx->scopes, i - 1), name);
        if (decl)
        {
            return decl;
        }
    }
    return NULL;
}

/* Look up only the innermost scope (for same-scope redeclaration checks). */
static ASTVarDecl *scope_top_lookup(SemanticCtx *ctx, const char *name)
{
    return strmap_get(current_scope(ctx), name);
}

static bool check_identifier_expr(ASTIdent *ident, SemanticCtx *ctx)
{
    ASTVarDecl *decl = scope_lookup(ctx, ident->name);
    if (!decl)
    {
        decl = strmap_get(ctx->global_vars, ident->name);
    }
    if (!decl)
    {
        sem_error(ident->base.loc, "undeclared identifier '%s'", ident->name);
        ctx->error = true;
        return false;
    }
    ident->decl = decl;
    ident->base.expr_type = type_decay(decl->type);
    return true;
}

static bool is_comparison_op(BinOpKind op)
{
    return op >= BIN_EQ && op <= BIN_GE;
}

/* C11 §6.5.16.1p1 assignment compatibility, used by `=`, call arguments,
   returns, and initializers. Pointers: the pointee types must match after
   stripping qualifiers, and the left may only *gain* qualifiers at the first
   pointee level (adding const is fine; discarding it is a constraint
   violation; deeper pointer levels must match exactly — `int**` is not
   assignable to `const int**`). Records: identical unqualified type. Other
   scalar conversions are permitted (width conversions happen at IR lowering). */
static bool type_assignable(Type *dst, Type *src)
{
    dst = type_unqual(dst);
    src = type_unqual(src);
    if (type_is_ptr(dst) && type_is_ptr(src))
    {
        Type *pd = type_deref(dst);
        Type *ps = type_deref(src);
        if (pd->kind == TYPE_VOID || ps->kind == TYPE_VOID)
        {
            return true;
        }
        if (pd->kind == TYPE_PTR || ps->kind == TYPE_PTR)
        {
            return pd == ps;
        }
        if (type_unqual(pd) != type_unqual(ps))
        {
            return false;
        }
        return !type_is_const(ps) || type_is_const(pd);
    }
    if (type_is_record(dst) && type_is_record(src))
    {
        return dst == src;
    }
    if (type_is_record(dst) || type_is_record(src))
    {
        return false;
    }
    return true;
}

static bool check_binary_expr(ASTBinaryExpr *binary_expr, SemanticCtx *ctx)
{
    if (!check_expr(binary_expr->left, ctx) || !check_expr(binary_expr->right, ctx))
    {
        return false;
    }
    Type *lt = binary_expr->left->expr_type;
    Type *rt = binary_expr->right->expr_type;
    Type *result = NULL;
    if (!check_value_used(binary_expr->left, ctx) || !check_value_used(binary_expr->right, ctx))
    {
        /* `(void)x + 1`, `f() = 5` — operand is void, not a value. */
        return false;
    }
    if ((type_is_record(lt) || type_is_record(rt)) && binary_expr->op != BIN_ASSIGN)
    {
        sem_error(binary_expr->base.loc, "invalid operands to operator (record type)");
        ctx->error = true;
        return false;
    }
    if (binary_expr->op == BIN_ASSIGN)
    {
        ASTNode *lhs = binary_expr->left;
        bool is_lvalue = lhs->kind == AST_IDENT || lhs->kind == AST_MEMBER_ACCESS ||
                         lhs->kind == AST_SUBSCRIPT_EXPR ||
                         (lhs->kind == AST_UNARY_EXPR && ast_as(ASTUnaryExpr, lhs)->op == UN_DEREF);
        if (!is_lvalue)
        {
            sem_error(binary_expr->base.loc, "lvalue required as left operand of assignment");
            ctx->error = true;
            return false;
        }
        if (type_is_const(lt))
        {
            sem_error(binary_expr->base.loc,
                      "assignment to const-qualified lvalue (read-only object)");
            ctx->error = true;
            return false;
        }
        if (type_is_ptr(lt) && type_is_ptr(rt) && !type_assignable(lt, rt))
        {
            sem_error(binary_expr->base.loc, "incompatible pointer types in assignment");
            ctx->error = true;
            return false;
        }
        if ((type_is_record(lt) || type_is_record(rt)) && !type_assignable(lt, rt))
        {
            sem_error(binary_expr->base.loc, "incompatible types in struct/union assignment");
            ctx->error = true;
            return false;
        }
        result = type_rvalue(lt);
    }
    else
    {
        lt = type_rvalue(lt);
        rt = type_rvalue(rt);
        if (binary_expr->op == BIN_LOG_AND || binary_expr->op == BIN_LOG_OR)
        {
            result = type_int();
        }
        else if (is_comparison_op(binary_expr->op))
        {
            result = type_int();
        }
        else if (type_is_ptr(lt) && (binary_expr->op == BIN_ADD || binary_expr->op == BIN_SUB) &&
                 !type_is_ptr(rt))
        {
            result = lt;
        }
        else
        {
            result = type_common(type_promote(lt), type_promote(rt));
        }
    }
    binary_expr->base.expr_type = result;
    return true;
}

static bool check_unary_expr(ASTUnaryExpr *unary_expr, SemanticCtx *ctx)
{
    if (!check_expr(unary_expr->operand, ctx))
    {
        return false;
    }
    Type *op_type = unary_expr->operand->expr_type;
    if (unary_expr->op == UN_LOG_NOT)
    {
        if (!check_value_used(unary_expr->operand, ctx))
        {
            return false;
        }
        unary_expr->base.expr_type = type_int();
        return true;
    }
    if (unary_expr->op == UN_DEREF)
    {
        if (!type_is_ptr(op_type))
        {
            sem_error(unary_expr->base.loc, "cannot dereference non-pointer type");
            ctx->error = true;
            return false;
        }
        /* Lvalue: the pointee type carries the const (const int* -> const int). */
        unary_expr->base.expr_type = type_deref(op_type);
        return true;
    }
    if (unary_expr->op == UN_ADDR)
    {
        ASTNode *operand = unary_expr->operand;
        /* Address-of yields a pointer to the operand's *declared* (lvalue)
           type, qualifiers included: &const_x is `const int*`. */
        if (operand->kind == AST_UNARY_EXPR && ast_as(ASTUnaryExpr, operand)->op == UN_DEREF)
        {
            unary_expr->base.expr_type = type_ptr(op_type);
            return true;
        }
        if (operand->kind == AST_SUBSCRIPT_EXPR)
        {
            unary_expr->base.expr_type = type_ptr(op_type);
            return true;
        }
        if (operand->kind == AST_MEMBER_ACCESS)
        {
            unary_expr->base.expr_type = type_ptr(op_type);
            return true;
        }
        if (operand->kind == AST_IDENT)
        {
            ASTVarDecl *decl = ast_as(ASTIdent, operand)->decl;
            /* Any lvalue identifier is addressable: file globals and block
               statics are address constants, and block-scope autos spill to a
               stack slot (Phase 9b). */
            if (!decl)
            {
                sem_error(unary_expr->base.loc, "cannot take address of this expression");
                ctx->error = true;
                return false;
            }
            unary_expr->base.expr_type = type_ptr(type_decay(decl->type));
            return true;
        }
        sem_error(unary_expr->base.loc, "cannot take address of this expression");
        ctx->error = true;
        return false;
    }
    if (op_type->kind == TYPE_VOID)
    {
        sem_error(unary_expr->base.loc, "void value not ignored as it ought to be");
        ctx->error = true;
        return false;
    }
    if (type_is_record(op_type))
    {
        sem_error(unary_expr->base.loc, "invalid operand of record type to unary operator");
        ctx->error = true;
        return false;
    }
    unary_expr->base.expr_type = type_promote(type_rvalue(op_type));
    return true;
}

static bool check_call_expr(ASTCallExpr *call_expr, SemanticCtx *ctx)
{
    ASTFuncDef *callee = strmap_get(ctx->globals, call_expr->callee);
    if (!callee)
    {
        sem_error(call_expr->base.loc, "undeclared function '%s'", call_expr->callee);
        ctx->error = true;
        return false;
    }

    size_t expected = vec_size(callee->params);
    size_t got = vec_size(call_expr->args);
    if (expected != got)
    {
        sem_error(call_expr->base.loc, "function '%s' expects %zu arguments, got %zu",
                  call_expr->callee, expected, got);
        ctx->error = true;
        return false;
    }

    size_t nargs = vec_size(call_expr->args);
    for (size_t i = 0; i < nargs; i++)
    {
        ASTNode *arg = (ASTNode *) vec_get(call_expr->args, i);
        if (!check_expr(arg, ctx))
        {
            return false;
        }
        if (!check_value_used(arg, ctx))
        {
            return false;
        }
        ASTVarDecl *param = ast_as(ASTVarDecl, (ASTNode *) vec_get(callee->params, i));
        if (!type_assignable(param->type, arg->expr_type))
        {
            sem_error(arg->loc, "incompatible argument type for parameter '%s'", param->name);
            ctx->error = true;
            return false;
        }
    }
    /* The function value is an unqualified rvalue even for a const return
       type (`const int f()`). */
    call_expr->base.expr_type = type_rvalue(callee->ret_type);
    return true;
}

static bool check_cast_expr(ASTCastExpr *ce, SemanticCtx *ctx)
{
    if (!check_expr(ce->operand, ctx))
    {
        return false;
    }
    Type *target = ce->target_type;
    Type *op = ce->operand->expr_type;
    if (target->kind == TYPE_VOID)
    {
        /* `(void) expr` discards the operand's value (C11 §6.5.4p2: the
           non-scalar-target constraint applies only when the type name is not
           void), so any operand type is legal. */
        ce->base.expr_type = target;
        return true;
    }
    if (!is_scalar_type(target))
    {
        sem_error(ce->base.loc, "conversion to non-scalar type requested");
        ctx->error = true;
        return false;
    }
    if (!is_scalar_type(op))
    {
        sem_error(ce->base.loc, "invalid cast of non-scalar type");
        ctx->error = true;
        return false;
    }
    /* A cast does not yield an lvalue, and a cast to a qualified type has the
       same effect as a cast to its unqualified version (C11 §6.5.4p4): the
       result is the rvalue of the target, so top-level `const` is dropped
       while pointee qualifiers survive (`(const int *)p` stays
       pointer-to-const-int). */
    ce->base.expr_type = type_rvalue(target);
    return true;
}

static bool check_member_access(ASTMemberAccess *ma, SemanticCtx *ctx)
{
    if (!check_expr(ma->object, ctx))
    {
        return false;
    }
    Type *obj_type = ma->object->expr_type;
    Type *record_type;
    if (ma->is_arrow)
    {
        if (!type_is_ptr(obj_type))
        {
            sem_error(ma->base.loc, "cannot use '->' on non-pointer type");
            ctx->error = true;
            return false;
        }
        record_type = type_deref(obj_type);
    }
    else
    {
        record_type = obj_type;
    }
    if (!type_is_record(record_type))
    {
        sem_error(ma->base.loc, "member access on non-struct/union type");
        ctx->error = true;
        return false;
    }
    if (!type_is_complete(record_type))
    {
        sem_error(ma->base.loc, "member access on incomplete type '%s'", record_type->record.tag);
        ctx->error = true;
        return false;
    }
    Type *field_type = type_record_field(record_type, ma->member);
    if (!field_type)
    {
        sem_error(ma->base.loc, "no member named '%s'", ma->member);
        ctx->error = true;
        return false;
    }
    /* C11 §6.5.2.3p4: member access on a const-qualified object (or through a
       pointer to one) yields a const-qualified member lvalue. For array
       members the qualifier lands on the element via type_const, so `s.a[i]`
       writes are caught and decay gives `const T*`. */
    ma->field_offset = type_record_field_offset(record_type, ma->member);
    ma->field_type = field_type;
    if (type_is_const(record_type))
    {
        ma->base.expr_type = type_decay(type_const(field_type));
    }
    else
    {
        ma->base.expr_type = type_decay(field_type);
    }
    return true;
}

static Type *check_expr(ASTNode *node, SemanticCtx *ctx)
{
    switch (node->kind)
    {
        case AST_INT_LITERAL:
        {
            ASTIntLiteral *lit = ast_as(ASTIntLiteral, node);
            node->expr_type =
                type_int_literal(lit->value, lit->is_hex, lit->is_unsigned, lit->length);
            return node->expr_type;
        }
        case AST_IDENT:
            if (!check_identifier_expr(ast_as(ASTIdent, node), ctx))
            {
                return NULL;
            }
            return node->expr_type;
        case AST_BINARY_EXPR:
            if (!check_binary_expr(ast_as(ASTBinaryExpr, node), ctx))
            {
                return NULL;
            }
            return node->expr_type;
        case AST_UNARY_EXPR:
            if (!check_unary_expr(ast_as(ASTUnaryExpr, node), ctx))
            {
                return NULL;
            }
            return node->expr_type;
        case AST_CALL_EXPR:
            if (!check_call_expr(ast_as(ASTCallExpr, node), ctx))
            {
                return NULL;
            }
            return node->expr_type;
        case AST_TERNARY_EXPR:
            if (!check_ternary_expression(ast_as(ASTTernaryExpr, node), ctx))
            {
                return NULL;
            }
            return node->expr_type;
        case AST_SUBSCRIPT_EXPR:
        {
            ASTSubscriptExpr *se = ast_as(ASTSubscriptExpr, node);
            if (!check_expr(se->array, ctx) || !check_expr(se->index, ctx))
            {
                return NULL;
            }
            Type *arr_type = se->array->expr_type;
            Type *ptr_type = type_decay(arr_type);
            if (!type_is_ptr(ptr_type))
            {
                sem_error(node->loc, "subscripted value is not a pointer or array");
                ctx->error = true;
                return NULL;
            }
            node->expr_type = type_deref(ptr_type);
            return node->expr_type;
        }
        case AST_SIZEOF_EXPR:
        {
            ASTSizeofExpr *se = ast_as(ASTSizeofExpr, node);
            if (!check_expr(se->operand, ctx))
            {
                return NULL;
            }
            Type *op_type = se->operand->expr_type;
            /* §6.3.2.1p3: array-to-pointer decay is suppressed for the direct
               operand of sizeof, so `sizeof(arr)` is the whole array size. */
            if (se->operand->kind == AST_IDENT)
            {
                ASTVarDecl *decl = ast_as(ASTIdent, se->operand)->decl;
                if (decl && type_is_array(decl->type))
                {
                    op_type = decl->type;
                }
            }
            if (op_type->kind == TYPE_VOID)
            {
                sem_error(node->loc, "sizeof(void) is invalid");
                ctx->error = true;
                return NULL;
            }
            if (type_is_record(op_type) && !type_is_complete(op_type))
            {
                sem_error(node->loc, "sizeof of incomplete type");
                ctx->error = true;
                return NULL;
            }
            se->size_value = type_sizeof(op_type);
            node->expr_type = type_ulong();
            return node->expr_type;
        }
        case AST_SIZEOF_TYPE:
        {
            ASTSizeofType *st = ast_as(ASTSizeofType, node);
            if (st->type->kind == TYPE_VOID)
            {
                sem_error(node->loc, "sizeof(void) is invalid");
                ctx->error = true;
                return NULL;
            }
            if (type_is_record(st->type) && !type_is_complete(st->type))
            {
                sem_error(node->loc, "sizeof of incomplete type");
                ctx->error = true;
                return NULL;
            }
            st->size_value = type_sizeof(st->type);
            node->expr_type = type_ulong();
            return node->expr_type;
        }
        case AST_STRING_LITERAL:
        {
            ASTStringLiteral *sl = ast_as(ASTStringLiteral, node);
            node->expr_type = type_decay(type_array(type_char(), sl->length + 1));
            return node->expr_type;
        }
        case AST_MEMBER_ACCESS:
            if (!check_member_access(ast_as(ASTMemberAccess, node), ctx))
            {
                return NULL;
            }
            return node->expr_type;
        case AST_CAST_EXPR:
            if (!check_cast_expr(ast_as(ASTCastExpr, node), ctx))
            {
                return NULL;
            }
            return node->expr_type;
        default:
            sem_error(node->loc, "unsupported expression kind %s", ast_kind_name(node->kind));
            ctx->error = true;
            return NULL;
    }
}

static bool check_return_stmt(ASTReturnStmt *return_stmt, SemanticCtx *ctx, Type *ret_type)
{
    if (ret_type->kind == TYPE_VOID)
    {
        if (return_stmt->expr)
        {
            sem_error(return_stmt->base.loc, "void function should not return a value");
            ctx->error = true;
            return false;
        }
    }
    else
    {
        if (!return_stmt->expr)
        {
            sem_error(return_stmt->base.loc, "non-void function must return a value");
            ctx->error = true;
            return false;
        }
    }
    if (return_stmt->expr && !check_expr(return_stmt->expr, ctx))
    {
        return false;
    }
    if (return_stmt->expr && !check_value_used(return_stmt->expr, ctx))
    {
        return false;
    }
    if (return_stmt->expr && type_is_record(ret_type) &&
        !type_assignable(ret_type, return_stmt->expr->expr_type))
    {
        sem_error(return_stmt->base.loc,
                  "returning a value incompatible with struct/union return type '%s'",
                  ret_type->record.tag);
        ctx->error = true;
        return false;
    }
    if (return_stmt->expr && type_is_ptr(ret_type) && type_is_ptr(return_stmt->expr->expr_type) &&
        !type_assignable(ret_type, return_stmt->expr->expr_type))
    {
        sem_error(return_stmt->base.loc, "incompatible pointer type in return");
        ctx->error = true;
        return false;
    }

    return true;
}

static bool check_variable_declaration(ASTVarDecl *var_decl, SemanticCtx *ctx)
{
    if (scope_top_lookup(ctx, var_decl->name))
    {
        sem_error(var_decl->base.loc, "redeclaration of '%s'", var_decl->name);
        ctx->error = true;
        {
            return false;
        }
    }
    if (var_decl->type->kind == TYPE_VOID)
    {
        sem_error(var_decl->base.loc, "variable '%s' has void type", var_decl->name);
        ctx->error = true;
        return false;
    }
    if (var_decl->storage == SC_STATIC)
    {
        /* Block-scope statics are file-backed objects: the parser folded a
           constant initializer into const_init, routed a brace list or a
           `char *` string literal into init; everything else is rejected.
           Lists and char arrays get a plan; the serializer emits the bytes. */
        if (type_is_record(var_decl->type) && !type_is_complete(var_decl->type))
        {
            sem_error(var_decl->base.loc, "variable '%s' has incomplete type", var_decl->name);
            ctx->error = true;
            return false;
        }
        if (var_decl->init)
        {
            if (var_decl->init->kind == AST_INIT_LIST)
            {
                InitPlan *plan = init_plan_new(ctx, var_decl->type);
                if (!plan_list(ctx, plan, var_decl->type, ast_as(ASTInitList, var_decl->init), 0))
                {
                    return false;
                }
                var_decl->plan = plan;
            }
            else if (var_decl->init->kind == AST_STRING_LITERAL && type_is_array(var_decl->type))
            {
                if (!plan_char_array_from_string(ctx, var_decl))
                {
                    return false;
                }
            }
            else if (var_decl->init->kind == AST_UNARY_EXPR)
            {
                if (!plan_ptr_initializer(ctx, var_decl))
                {
                    return false;
                }
            }
            else if (!type_is_ptr(var_decl->type) || type_deref(var_decl->type)->kind != TYPE_CHAR)
            {
                sem_error(var_decl->base.loc,
                          "string-literal initializer requires a 'char *' variable");
                ctx->error = true;
                return false;
            }
        }
        if (var_decl->has_const_init &&
            (var_decl->type->kind == TYPE_ARRAY || type_is_record(var_decl->type)))
        {
            sem_error(var_decl->base.loc,
                      "aggregate '%s' must be initialized with a brace-enclosed list",
                      var_decl->name);
            ctx->error = true;
            return false;
        }
    }
    if (var_decl->storage == SC_EXTERN)
    {
        /* Block-scope extern declares the external-linkage entity (C11
           §6.2.2p5); it allocates no local storage and resolves through the
           file-scope/external namespace, not the block locals. */
        if (var_decl->init)
        {
            sem_error(var_decl->base.loc, "'%s' has both 'extern' and an initializer",
                      var_decl->name);
            ctx->error = true;
            return false;
        }
        if (strmap_get(ctx->globals, var_decl->name))
        {
            sem_error(var_decl->base.loc, "'%s' redeclared as different kind of symbol",
                      var_decl->name);
            ctx->error = true;
            return false;
        }
        ASTVarDecl *existing = strmap_get(ctx->global_vars, var_decl->name);
        if (existing && existing->storage == SC_STATIC)
        {
            sem_error(var_decl->base.loc,
                      "extern declaration of '%s' follows static "
                      "declaration",
                      var_decl->name);
            ctx->error = true;
            return false;
        }
        if (!existing)
        {
            strmap_set(ctx->global_vars, var_decl->name, var_decl);
        }
        return true;
    }
    var_decl->is_block_scope = true;
    strmap_set(current_scope(ctx), var_decl->name, var_decl);
    if (var_decl->init)
    {
        if (var_decl->init->kind == AST_INIT_LIST)
        {
            InitPlan *plan = init_plan_new(ctx, var_decl->type);
            if (!plan_list(ctx, plan, var_decl->type, ast_as(ASTInitList, var_decl->init), 0))
            {
                return false;
            }
            var_decl->plan = plan;
            return true;
        }
        if (var_decl->init->kind == AST_STRING_LITERAL && type_is_array(var_decl->type))
        {
            return plan_char_array_from_string(ctx, var_decl);
        }
        if (!check_expr(var_decl->init, ctx))
        {
            return false;
        }
        if (!check_value_used(var_decl->init, ctx))
        {
            return false;
        }
        if (type_is_record(var_decl->type) &&
            !type_assignable(var_decl->type, var_decl->init->expr_type))
        {
            sem_error(var_decl->base.loc, "invalid initializer for struct/union type '%s'",
                      var_decl->type->record.tag);
            ctx->error = true;
            return false;
        }
        if (type_is_ptr(var_decl->type) &&
            !type_assignable(var_decl->type, var_decl->init->expr_type))
        {
            sem_error(var_decl->base.loc, "incompatible pointer type in initializer for '%s'",
                      var_decl->name);
            ctx->error = true;
            return false;
        }
    }
    return true;
}

static bool check_expression_statement(ASTExprStmt *expr_stmt, SemanticCtx *ctx)
{
    return check_expr(expr_stmt->expr, ctx);
}

/* ---- initializer-list planner (C11 §6.7.9, D12.5) ----

   Flattens a brace-enclosed initializer tree into offset-targeted writes on
   the object being initialized. A cursor of aggregate frames walks the
   subobjects in initialization order; brace elision falls out of the descent:
   a scalar clause drills to the deepest leaf, a braced clause consumes a
   whole subtree. Designators rebind the cursor relative to this list's object
   (§6.7.9p18) so later undesignated clauses continue after the target. */

typedef struct
{
    Type *agg; /* array / struct / union type this frame iterates */
    u32 next;  /* next child index to consume within `agg` */
    u32 base;  /* absolute byte offset of this aggregate in the object */
} PlanFrame;

static bool is_aggregate_type(Type *t)
{
    t = type_unqual(t);
    return type_is_array(t) || type_is_record(t);
}

static u32 aggr_nchildren(Type *agg)
{
    agg = type_unqual(agg);
    if (type_is_array(agg))
    {
        return (u32) agg->arr.length;
    }
    return (u32) vec_size(agg->record.fields);
}

static bool aggr_child(Type *agg, u32 idx, u32 base, Type **cty, u32 *coff)
{
    agg = type_unqual(agg);
    if (type_is_array(agg))
    {
        if (idx >= agg->arr.length)
        {
            return false;
        }
        Type *elem = type_unqual(type_array_elem(agg));
        *cty = elem;
        *coff = base + (u32) (idx * agg->arr.elem->size);
        return true;
    }
    size_t nf = vec_size(agg->record.fields);
    if (idx >= nf)
    {
        return false;
    }
    RecordField *f = (RecordField *) vec_get(agg->record.fields, idx);
    *cty = type_unqual(f->type);
    *coff = base + f->offset; /* unions: every member sits at offset 0 */
    return true;
}

/* Walk the fields of `agg` looking for `name`; returns its index on success. */
static bool aggr_field_index(Type *agg, const char *name, u32 *out)
{
    agg = type_unqual(agg);
    size_t nf = vec_size(agg->record.fields);
    for (size_t i = 0; i < nf; i++)
    {
        RecordField *f = (RecordField *) vec_get(agg->record.fields, i);
        if (strcmp(f->name, name) == 0)
        {
            *out = (u32) i;
            return true;
        }
    }
    return false;
}

static void cursor_advance(Vec *stack)
{
    while (vec_size(stack) > 0)
    {
        PlanFrame *top = (PlanFrame *) vec_last(stack);
        top->next++;
        if (top->next < aggr_nchildren(top->agg))
        {
            return;
        }
        vec_pop(stack);
    }
}

static InitWrite *plan_new_write(SemanticCtx *ctx, InitPlan *plan, u32 offset, Type *type,
                                 ASTNode *value, bool is_string_fill)
{
    InitWrite *w = arena_alloc(ctx->arena, sizeof(InitWrite), _Alignof(InitWrite));
    w->offset = offset;
    w->type = type;
    w->value = value;
    w->is_string_fill = is_string_fill;
    vec_push(plan->writes, w);
    return w;
}

static InitPlan *init_plan_new(SemanticCtx *ctx, Type *obj_type)
{
    InitPlan *plan = arena_alloc(ctx->arena, sizeof(InitPlan), _Alignof(InitPlan));
    plan->writes = vec_new(ctx->arena);
    plan->total_size = type_sizeof(obj_type);
    return plan;
}

/* Validate + record a scalar write (initialization bypasses the §9 write gate,
   so const targets are fine — D12.10). */
static bool plan_scalar_write(SemanticCtx *ctx, InitPlan *plan, Type *target, u32 offset,
                              ASTNode *value, Loc loc)
{
    if (!check_expr(value, ctx))
    {
        return false;
    }
    if (!check_value_used(value, ctx))
    {
        return false;
    }
    if (!type_assignable(target, value->expr_type))
    {
        sem_error(loc, "incompatible type in initializer (target type differs)");
        ctx->error = true;
        return false;
    }
    plan_new_write(ctx, plan, offset, target, value, false);
    return true;
}

static bool resolve_designator_path(SemanticCtx *ctx, Type *t, u32 base_off, Designator *d,
                                    Vec *path, Type **out_ty, u32 *out_off, Loc loc)
{
    Type *cur_ty = type_unqual(t);
    u32 cur_off = base_off;
    for (Designator *dd = d; dd; dd = dd->next)
    {
        if (type_is_array(cur_ty))
        {
            if (dd->kind != ND_INDEX)
            {
                sem_error(loc, "field designator '.%s' used on an array object", dd->field);
                ctx->error = true;
                return false;
            }
            if (dd->index < 0 || (u64) dd->index >= cur_ty->arr.length)
            {
                sem_error(loc, "array designator index %lld is out of bounds",
                          (long long) dd->index);
                ctx->error = true;
                return false;
            }
            PlanFrame *fr = arena_alloc(ctx->arena, sizeof(PlanFrame), _Alignof(PlanFrame));
            fr->agg = cur_ty;
            fr->base = cur_off;
            fr->next = (u32) dd->index;
            vec_push(path, fr);
            Type *elem = type_unqual(type_array_elem(cur_ty));
            cur_off = cur_off + (u32) (dd->index * cur_ty->arr.elem->size);
            cur_ty = elem;
        }
        else if (type_is_record(cur_ty))
        {
            if (dd->kind != ND_INDEX)
            {
                u32 fidx;
                if (!aggr_field_index(cur_ty, dd->field, &fidx))
                {
                    sem_error(loc, "no member named '%s' in '%s'", dd->field, cur_ty->record.tag);
                    ctx->error = true;
                    return false;
                }
                RecordField *f = (RecordField *) vec_get(cur_ty->record.fields, fidx);
                PlanFrame *fr = arena_alloc(ctx->arena, sizeof(PlanFrame), _Alignof(PlanFrame));
                fr->agg = cur_ty;
                fr->base = cur_off;
                fr->next = fidx;
                vec_push(path, fr);
                cur_off = cur_off + f->offset;
                cur_ty = type_unqual(f->type);
            }
            else
            {
                sem_error(loc, "array designator used on a non-array object");
                ctx->error = true;
                return false;
            }
        }
        else
        {
            sem_error(loc, "cannot apply a designator to a scalar object");
            ctx->error = true;
            return false;
        }
    }
    *out_ty = cur_ty;
    *out_off = cur_off;
    return true;
}

static bool plan_list(SemanticCtx *ctx, InitPlan *plan, Type *t, ASTInitList *list, u32 base_off);

/* Handle a clause whose value is a (non-braced) string literal: it fills a
   whole `char[N]` subobject when it fits, otherwise it is an ordinary scalar
   write (a `char *` member, or an error). */
static bool plan_string_clause(SemanticCtx *ctx, InitPlan *plan, Type *cty, u32 coff,
                               ASTNode *value, Loc loc)
{
    if (type_is_array(cty))
    {
        if (cty->arr.length == 0)
        {
            sem_error(loc, "array has incomplete type");
            ctx->error = true;
            return false;
        }
        if (type_array_elem(cty)->kind != TYPE_CHAR)
        {
            sem_error(loc, "string literal only initializes a char array");
            ctx->error = true;
            return false;
        }
        ASTStringLiteral *sl = ast_as(ASTStringLiteral, value);
        if (sl->length + 1 > cty->arr.length)
        {
            sem_error(loc, "initializer-string for array of chars is too long");
            ctx->error = true;
            return false;
        }
        plan_new_write(ctx, plan, coff, cty, value, true);
        return true;
    }
    /* `char *` (or another pointer) member/array element: the string decays. */
    return plan_scalar_write(ctx, plan, cty, coff, value, loc);
}

static bool plan_list(SemanticCtx *ctx, InitPlan *plan, Type *t, ASTInitList *list, u32 base_off)
{
    t = type_unqual(t);
    size_t nel = vec_size(list->elems);
    if (nel == 0)
    {
        return true; /* `{}`: zero-init, nothing to write */
    }

    if (is_scalar_type(t))
    {
        if (nel != 1)
        {
            sem_error(list->base.loc, "excess elements in scalar initializer");
            ctx->error = true;
            return false;
        }
        InitElem *e = (InitElem *) vec_get(list->elems, 0);
        if (e->design)
        {
            sem_error(e->loc, "cannot use a designator with a scalar initializer");
            ctx->error = true;
            return false;
        }
        return plan_scalar_write(ctx, plan, t, base_off, e->value, e->loc);
    }

    if (type_is_array(t) && t->arr.length == 0)
    {
        sem_error(list->base.loc, "array has incomplete type");
        ctx->error = true;
        return false;
    }

    Vec *stack = vec_new(ctx->arena);
    PlanFrame *root = arena_alloc(ctx->arena, sizeof(PlanFrame), _Alignof(PlanFrame));
    root->agg = t;
    root->next = 0;
    root->base = base_off;
    vec_push(stack, root);

    for (size_t i = 0; i < nel; i++)
    {
        InitElem *e = (InitElem *) vec_get(list->elems, i);

        if (e->design)
        {
            Vec *path = vec_new(ctx->arena);
            Type *dt;
            u32 doff;
            if (!resolve_designator_path(ctx, t, base_off, e->design, path, &dt, &doff, e->loc))
            {
                return false;
            }
            stack = path;
        }

        if (vec_size(stack) == 0)
        {
            sem_error(e->loc, "excess elements in %s initializer",
                      type_is_array(t) ? "array" : "struct/union");
            ctx->error = true;
            return false;
        }

        PlanFrame *top = (PlanFrame *) vec_last(stack);
        Type *cty;
        u32 coff;
        if (!aggr_child(top->agg, top->next, top->base, &cty, &coff))
        {
            sem_error(e->loc, "excess elements in %s initializer",
                      type_is_array(t) ? "array" : "struct/union");
            ctx->error = true;
            return false;
        }

        if (e->value->kind == AST_INIT_LIST)
        {
            if (!plan_list(ctx, plan, cty, ast_as(ASTInitList, e->value), coff))
            {
                return false;
            }
            cursor_advance(stack);
        }
        else if (e->value->kind == AST_STRING_LITERAL)
        {
            if (!plan_string_clause(ctx, plan, cty, coff, e->value, e->loc))
            {
                return false;
            }
            cursor_advance(stack);
        }
        else
        {
            /* Scalar clause with brace elision: drill to the leaf subobject. */
            while (is_aggregate_type(cty))
            {
                PlanFrame *fr = arena_alloc(ctx->arena, sizeof(PlanFrame), _Alignof(PlanFrame));
                fr->agg = cty;
                fr->base = coff;
                fr->next = 0;
                vec_push(stack, fr);
                top = (PlanFrame *) vec_last(stack);
                if (!aggr_child(top->agg, top->next, top->base, &cty, &coff))
                {
                    sem_error(e->loc, "excess elements in %s initializer",
                              type_is_array(t) ? "array" : "struct/union");
                    ctx->error = true;
                    return false;
                }
            }
            if (!plan_scalar_write(ctx, plan, cty, coff, e->value, e->loc))
            {
                return false;
            }
            cursor_advance(stack);
        }
    }
    return true;
}

/* `char s[N] = "hi"`: the bytes incl. NUL must fit (C11 §6.7.9p14), and the
   rest of the array is zero-padded. Returns false on error and otherwise fills
   vd->plan with a single string-fill write. */
static bool plan_char_array_from_string(SemanticCtx *ctx, ASTVarDecl *vd)
{
    Type *arr = type_unqual(vd->type);
    ASTStringLiteral *sl = ast_as(ASTStringLiteral, vd->init);
    if (type_array_len(arr) == 0)
    {
        sem_error(vd->base.loc, "array '%s' has incomplete type", vd->name);
        ctx->error = true;
        return false;
    }
    if (type_array_elem(arr)->kind != TYPE_CHAR)
    {
        sem_error(vd->base.loc, "string-literal initializer requires a 'char' array");
        ctx->error = true;
        return false;
    }
    if (sl->length + 1 > type_array_len(arr))
    {
        sem_error(vd->base.loc, "initializer-string for array of chars is too long");
        ctx->error = true;
        return false;
    }
    InitPlan *plan = init_plan_new(ctx, arr);
    plan_new_write(ctx, plan, 0, arr, vd->init, true);
    vd->plan = plan;
    return true;
}

/* `int *p = &g;` (block static or file scope): a bare address constant in
   pointer position. The value type-checks against the pointer target; the IR
   serializer turns it into one 8-byte relocation write (§6.6p9, D12.8). */
static bool plan_ptr_initializer(SemanticCtx *ctx, ASTVarDecl *vd)
{
    if (vd->init->kind != AST_UNARY_EXPR)
    {
        return false;
    }
    ASTUnaryExpr *u = ast_as(ASTUnaryExpr, vd->init);
    if (u->op != UN_ADDR || u->operand->kind != AST_IDENT)
    {
        return false;
    }
    if (!type_is_ptr(vd->type))
    {
        return false;
    }
    InitPlan *plan = init_plan_new(ctx, vd->type);
    if (!plan_scalar_write(ctx, plan, type_unqual(vd->type), 0, vd->init, vd->base.loc))
    {
        return false;
    }
    vd->plan = plan;
    return true;
}

static bool check_compound_statement(ASTCompoundStmt *compound_stmt, SemanticCtx *ctx,
                                     Type *ret_type)
{
    push_scope(ctx);
    bool ok = true;
    size_t nstmts = vec_size(compound_stmt->stmts);
    for (size_t i = 0; i < nstmts; i++)
    {
        ASTNode *stmt = (ASTNode *) vec_get(compound_stmt->stmts, i);
        if (!check_stmt(stmt, ctx, ret_type))
        {
            ok = false;
            break;
        }
    }
    pop_scope(ctx);
    return ok;
}

static bool check_statement_list(Vec *stmts, SemanticCtx *ctx, Type *ret_type)
{
    size_t nstmts = vec_size(stmts);
    for (size_t i = 0; i < nstmts; i++)
    {
        if (!check_stmt((ASTNode *) vec_get(stmts, i), ctx, ret_type))
        {
            return false;
        }
    }
    return true;
}

static bool check_if_statement(ASTIfStmt *if_stmt, SemanticCtx *ctx, Type *ret_type)
{
    if (!check_expr(if_stmt->cond, ctx) || !check_value_used(if_stmt->cond, ctx))
    {
        return false;
    }
    if (!check_stmt(if_stmt->then_branch, ctx, ret_type))
    {
        return false;
    }
    if (if_stmt->else_branch && !check_stmt(if_stmt->else_branch, ctx, ret_type))
    {
        return false;
    }
    return true;
}

static bool check_while_statement(ASTWhileStmt *while_stmt, SemanticCtx *ctx, Type *ret_type)
{
    if (!check_expr(while_stmt->cond, ctx) || !check_value_used(while_stmt->cond, ctx))
    {
        return false;
    }
    ctx->loop_depth++;
    bool ok = check_stmt(while_stmt->body, ctx, ret_type);
    ctx->loop_depth--;
    return ok;
}

static bool check_do_while_statement(ASTDoWhileStmt *do_stmt, SemanticCtx *ctx, Type *ret_type)
{
    ctx->loop_depth++;
    bool ok = check_stmt(do_stmt->body, ctx, ret_type);
    ctx->loop_depth--;
    if (!ok)
    {
        return false;
    }
    if (!check_expr(do_stmt->cond, ctx) || !check_value_used(do_stmt->cond, ctx))
    {
        return false;
    }
    return true;
}

static bool check_for_statement(ASTForStmt *for_stmt, SemanticCtx *ctx, Type *ret_type)
{
    if (for_stmt->init && !check_stmt(for_stmt->init, ctx, ret_type))
    {
        return false;
    }
    if (for_stmt->cond &&
        (!check_expr(for_stmt->cond, ctx) || !check_value_used(for_stmt->cond, ctx)))
    {
        return false;
    }
    if (for_stmt->post && !check_expr(for_stmt->post, ctx))
    {
        return false;
    }
    ctx->loop_depth++;
    bool ok = check_stmt(for_stmt->body, ctx, ret_type);
    ctx->loop_depth--;
    return ok;
}

static bool fold_integer_constant(ASTNode *node, i64 *out)
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
            if (!fold_integer_constant(u->operand, &v))
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
            if (!fold_integer_constant(b->left, &l) || !fold_integer_constant(b->right, &r))
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
            if (!fold_integer_constant(te->cond, &cond))
            {
                return false;
            }
            return cond ? fold_integer_constant(te->then_expr, out)
                        : fold_integer_constant(te->else_expr, out);
        }
        case AST_SIZEOF_TYPE:
            /* check_expr has already computed size_value. */
            *out = (i64) ast_as(ASTSizeofType, node)->size_value;
            return true;
        case AST_SIZEOF_EXPR:
            /* check_expr has already resolved the operand type and set
               size_value, so `case sizeof(x):` works here. */
            *out = (i64) ast_as(ASTSizeofExpr, node)->size_value;
            return true;
        case AST_CAST_EXPR:
        {
            /* Casts are permitted in integer constant expressions (§6.6p6);
               operand types are known by now, so `case (int)sizeof(x):`
               folds here. Only integer targets fold to an integer constant. */
            ASTCastExpr *ce = ast_as(ASTCastExpr, node);
            i64 v;
            if (!fold_integer_constant(ce->operand, &v) || !type_is_integer(ce->target_type))
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

static bool check_switch_statement(ASTSwitchStmt *sw, SemanticCtx *ctx, Type *ret_type)
{
    if (!check_expr(sw->cond, ctx))
    {
        return false;
    }
    if (!type_is_integer(type_rvalue(sw->cond->expr_type)))
    {
        sem_error(sw->cond->loc, "switch condition must have integer type");
        ctx->error = true;
        return false;
    }

    SwitchSem sem = {.values = u64map_new(ctx->arena),
                     .promoted_cond = type_promote(type_rvalue(sw->cond->expr_type)),
                     .has_default = false};
    vec_push(ctx->switch_sem_stack, &sem);
    ctx->switch_depth++;
    bool ok = check_stmt(sw->body, ctx, ret_type);
    ctx->switch_depth--;
    vec_pop(ctx->switch_sem_stack);
    return ok;
}

static bool check_case_statement(ASTCaseStmt *cs, SemanticCtx *ctx, Type *ret_type)
{
    if (ctx->switch_depth == 0)
    {
        sem_error(cs->base.loc, "'case' label not within a switch statement");
        ctx->error = true;
        return false;
    }
    SwitchSem *sem = (SwitchSem *) vec_last(ctx->switch_sem_stack);
    if (!cs->value_known)
    {
        /* The constant expression couldn't be folded at parse time (e.g.
           `case sizeof(x):`). Resolve types/sizes, then evaluate now. */
        if (!check_expr(cs->expr, ctx) || !fold_integer_constant(cs->expr, &cs->value))
        {
            sem_error(cs->base.loc, "case label is not an integer constant expression");
            ctx->error = true;
            return false;
        }
        cs->value_known = true;
    }
    cs->value = type_reduce_int(sem->promoted_cond, cs->value);
    if (u64map_get(sem->values, (u64) cs->value))
    {
        sem_error(cs->base.loc, "duplicate case value");
        ctx->error = true;
        return false;
    }
    u64map_set(sem->values, (u64) cs->value, (void *) 1);
    return check_statement_list(cs->stmts, ctx, ret_type);
}

static bool check_default_statement(ASTDefaultStmt *ds, SemanticCtx *ctx, Type *ret_type)
{
    if (ctx->switch_depth == 0)
    {
        sem_error(ds->base.loc, "'default' label not within a switch statement");
        ctx->error = true;
        return false;
    }
    SwitchSem *sem = (SwitchSem *) vec_last(ctx->switch_sem_stack);
    if (sem->has_default)
    {
        sem_error(ds->base.loc, "multiple default labels in one switch");
        ctx->error = true;
        return false;
    }
    sem->has_default = true;
    return check_statement_list(ds->stmts, ctx, ret_type);
}

static bool check_break_statement(ASTBreakStmt *break_stmt, SemanticCtx *ctx)
{
    (void) break_stmt;
    if (ctx->loop_depth == 0 && ctx->switch_depth == 0)
    {
        sem_error(break_stmt->base.loc, "'break' not within a loop or switch");
        ctx->error = true;
        return false;
    }
    return true;
}

static bool check_continue_statement(ASTContinueStmt *continue_stmt, SemanticCtx *ctx)
{
    (void) continue_stmt;
    if (ctx->loop_depth == 0)
    {
        sem_error(continue_stmt->base.loc, "'continue' outside of loop");
        ctx->error = true;
        return false;
    }
    return true;
}

static bool check_goto_statement(ASTGotoStmt *goto_stmt, SemanticCtx *ctx)
{
    if (!strmap_get(ctx->labels, goto_stmt->label))
    {
        sem_error(goto_stmt->base.loc, "undefined label '%s'", goto_stmt->label);
        ctx->error = true;
        return false;
    }
    return true;
}

static bool check_label_statement(ASTLabelStmt *label_stmt, SemanticCtx *ctx, Type *ret_type)
{
    /* Labels are pre-collected so forward gotos are valid; duplicates were
       already reported during collection. */
    (void) ctx;
    return check_stmt(label_stmt->stmt, ctx, ret_type);
}

static bool check_ternary_expression(ASTTernaryExpr *ternary, SemanticCtx *ctx)
{
    if (!check_expr(ternary->cond, ctx) || !check_expr(ternary->then_expr, ctx) ||
        !check_expr(ternary->else_expr, ctx))
    {
        return false;
    }
    if (!check_value_used(ternary->cond, ctx) || !check_value_used(ternary->then_expr, ctx) ||
        !check_value_used(ternary->else_expr, ctx))
    {
        return false;
    }
    Type *tt = type_rvalue(ternary->then_expr->expr_type);
    Type *te = type_rvalue(ternary->else_expr->expr_type);
    if (type_is_record(tt) || type_is_record(te))
    {
        sem_error(ternary->base.loc, "conditional operator on record type");
        ctx->error = true;
        return false;
    }
    ternary->base.expr_type = type_common(type_promote(tt), type_promote(te));
    return true;
}

/* A typedef names an existing (interned) type — it introduces no new type and
   no storage (C11 §6.7.7). The parser has already registered the name and
   resolved every later use through the interned `Type`, so semantic's job here
   is only to validate the *target*. Everything the language can legally
   typedef — scalar types, pointers, records (complete or forward), enums,
   even void (`typedef void V; V *p;`) — is valid; the meaningful checks live
   at the use site (a `void` variable, an incomplete object, etc.). Nothing
   else to do yet, but the node must not reach the loud-default error. */
static bool check_typedef_decl(ASTTypedefDecl *td, SemanticCtx *ctx)
{
    if (td->type->kind == TYPE_FUNC)
    {
        sem_error(td->base.loc, "typedef of a function type is not supported");
        ctx->error = true;
        return false;
    }
    (void) ctx;
    return true;
}

static bool check_stmt(ASTNode *node, SemanticCtx *ctx, Type *ret_type)
{
    switch (node->kind)
    {
        case AST_RETURN_STMT:
            return check_return_stmt(ast_as(ASTReturnStmt, node), ctx, ret_type);
        case AST_VAR_DECL:
            return check_variable_declaration(ast_as(ASTVarDecl, node), ctx);
        case AST_TYPEDEF_DECL:
            return check_typedef_decl(ast_as(ASTTypedefDecl, node), ctx);
        case AST_EXPR_STMT:
            return check_expression_statement(ast_as(ASTExprStmt, node), ctx);
        case AST_COMPOUND_STMT:
            return check_compound_statement(ast_as(ASTCompoundStmt, node), ctx, ret_type);
        case AST_IF_STMT:
            return check_if_statement(ast_as(ASTIfStmt, node), ctx, ret_type);
        case AST_WHILE_STMT:
            return check_while_statement(ast_as(ASTWhileStmt, node), ctx, ret_type);
        case AST_DO_WHILE_STMT:
            return check_do_while_statement(ast_as(ASTDoWhileStmt, node), ctx, ret_type);
        case AST_FOR_STMT:
            return check_for_statement(ast_as(ASTForStmt, node), ctx, ret_type);
        case AST_SWITCH_STMT:
            return check_switch_statement(ast_as(ASTSwitchStmt, node), ctx, ret_type);
        case AST_CASE_STMT:
            return check_case_statement(ast_as(ASTCaseStmt, node), ctx, ret_type);
        case AST_DEFAULT_STMT:
            return check_default_statement(ast_as(ASTDefaultStmt, node), ctx, ret_type);
        case AST_BREAK_STMT:
            return check_break_statement(ast_as(ASTBreakStmt, node), ctx);
        case AST_CONTINUE_STMT:
            return check_continue_statement(ast_as(ASTContinueStmt, node), ctx);
        case AST_GOTO_STMT:
            return check_goto_statement(ast_as(ASTGotoStmt, node), ctx);
        case AST_LABEL_STMT:
            return check_label_statement(ast_as(ASTLabelStmt, node), ctx, ret_type);
        default:
            sem_error(node->loc, "unsupported statement kind %s", ast_kind_name(node->kind));
            ctx->error = true;
            return false;
    }
}

static bool setup_function_params(ASTFuncDef *func_def, SemanticCtx *ctx)
{
    /* Parameters live in the function's outermost scope, which check_func has
       already pushed. */
    size_t nparams = vec_size(func_def->params);
    for (size_t i = 0; i < nparams; i++)
    {
        ASTVarDecl *param = ast_as(ASTVarDecl, (ASTNode *) vec_get(func_def->params, i));
        if (scope_top_lookup(ctx, param->name))
        {
            sem_error(param->base.loc, "redeclaration of parameter '%s'", param->name);
            ctx->error = true;
            return false;
        }
        if (param->type->kind == TYPE_VOID)
        {
            sem_error(param->base.loc, "parameter '%s' has void type", param->name);
            ctx->error = true;
            return false;
        }
        param->is_block_scope = true;
        strmap_set(current_scope(ctx), param->name, param);
    }
    return true;
}

static bool check_function_body(ASTFuncDef *func_def, SemanticCtx *ctx)
{
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, func_def->body);
    size_t nstmts = vec_size(body->stmts);
    for (size_t i = 0; i < nstmts; i++)
    {
        ASTNode *stmt = (ASTNode *) vec_get(body->stmts, i);
        if (!check_stmt(stmt, ctx, func_def->ret_type))
        {
            return false;
        }
    }
    return true;
}

/* Forward declaration for recursive statement traversal. */
static void collect_labels(ASTNode *node, SemanticCtx *ctx);

static void collect_labels_compound(ASTCompoundStmt *cs, SemanticCtx *ctx)
{
    size_t n = vec_size(cs->stmts);
    for (size_t i = 0; i < n; i++)
    {
        ASTNode *stmt = (ASTNode *) vec_get(cs->stmts, i);
        collect_labels(stmt, ctx);
    }
}

static void collect_labels(ASTNode *node, SemanticCtx *ctx)
{
    if (!node)
    {
        return;
    }
    switch (node->kind)
    {
        case AST_COMPOUND_STMT:
            collect_labels_compound(ast_as(ASTCompoundStmt, node), ctx);
            break;
        case AST_IF_STMT:
        {
            ASTIfStmt *is = ast_as(ASTIfStmt, node);
            collect_labels(is->then_branch, ctx);
            collect_labels(is->else_branch, ctx);
            break;
        }
        case AST_WHILE_STMT:
        {
            ASTWhileStmt *ws = ast_as(ASTWhileStmt, node);
            collect_labels(ws->body, ctx);
            break;
        }
        case AST_DO_WHILE_STMT:
        {
            ASTDoWhileStmt *ds = ast_as(ASTDoWhileStmt, node);
            collect_labels(ds->body, ctx);
            break;
        }
        case AST_FOR_STMT:
        {
            ASTForStmt *fs = ast_as(ASTForStmt, node);
            collect_labels(fs->body, ctx);
            break;
        }
        case AST_LABEL_STMT:
        {
            ASTLabelStmt *ls = ast_as(ASTLabelStmt, node);
            if (strmap_get(ctx->labels, ls->label))
            {
                sem_error(ls->base.loc, "redefinition of label '%s'", ls->label);
                ctx->error = true;
            }
            else
            {
                strmap_set(ctx->labels, ls->label, ls);
            }
            collect_labels(ls->stmt, ctx);
            break;
        }
        case AST_SWITCH_STMT:
        {
            ASTSwitchStmt *sw = ast_as(ASTSwitchStmt, node);
            collect_labels(sw->body, ctx);
            break;
        }
        case AST_CASE_STMT:
        {
            ASTCaseStmt *cs2 = ast_as(ASTCaseStmt, node);
            size_t n2 = vec_size(cs2->stmts);
            for (size_t i2 = 0; i2 < n2; i2++)
            {
                collect_labels((ASTNode *) vec_get(cs2->stmts, i2), ctx);
            }
            break;
        }
        case AST_DEFAULT_STMT:
        {
            ASTDefaultStmt *ds2 = ast_as(ASTDefaultStmt, node);
            size_t n2 = vec_size(ds2->stmts);
            for (size_t i2 = 0; i2 < n2; i2++)
            {
                collect_labels((ASTNode *) vec_get(ds2->stmts, i2), ctx);
            }
            break;
        }
        default:
            break;
    }
}

static bool check_func(ASTNode *node, SemanticCtx *ctx)
{
    ASSERT(node->kind == AST_FUNC_DEF);
    ASTFuncDef *fn = ast_as(ASTFuncDef, node);

    int saved_loop_depth = ctx->loop_depth;
    StrMap *saved_labels = ctx->labels;
    ctx->loop_depth = 0;
    ctx->labels = strmap_new(ctx->arena);

    push_scope(ctx);
    if (!setup_function_params(fn, ctx))
    {
        pop_scope(ctx);
        ctx->loop_depth = saved_loop_depth;
        ctx->labels = saved_labels;
        return false;
    }

    /* Labels may be referenced before they are defined (goto can jump forward),
       so collect them before checking the function body. */
    collect_labels(fn->body, ctx);

    bool result = check_function_body(fn, ctx);
    if (ctx->error)
    {
        /* An error was reported (e.g. duplicate label during collection);
           fail so the pipeline aborts instead of continuing to IR. */
        result = false;
    }

    pop_scope(ctx);
    ctx->loop_depth = saved_loop_depth;
    ctx->labels = saved_labels;
    return result;
}

/* Pass 1: Collect all file-scope variables, enforcing linkage rules (D8.3):
   repeated tentative/extern declarations merge; two definitions with a
   constant initializer collide. */
static bool collect_global_variables(ASTProgram *prog, SemanticCtx *ctx)
{
    size_t ndecls = vec_size(prog->decls);
    for (size_t i = 0; i < ndecls; i++)
    {
        ASTNode *decl = (ASTNode *) vec_get(prog->decls, i);
        if (decl->kind != AST_VAR_DECL)
        {
            continue;
        }
        ASTVarDecl *vd = ast_as(ASTVarDecl, decl);

        if (strmap_get(ctx->globals, vd->name))
        {
            sem_error(vd->base.loc, "redefinition of '%s' as a global variable", vd->name);
            ctx->error = true;
            return false;
        }
        if (vd->type->kind == TYPE_VOID)
        {
            sem_error(vd->base.loc, "variable '%s' has void type", vd->name);
            ctx->error = true;
            return false;
        }
        if (type_is_record(vd->type) && !type_is_complete(vd->type))
        {
            sem_error(vd->base.loc, "variable '%s' has incomplete type", vd->name);
            ctx->error = true;
            return false;
        }
        if (vd->init)
        {
            /* Initializer lists are flattened by the planner; char arrays take
               a string literal byte-fill; bare `char *` pointers keep the
               .data string-address relocation (D12.6/D12.8). */
            if (vd->init->kind == AST_INIT_LIST)
            {
                InitPlan *plan = init_plan_new(ctx, vd->type);
                if (!plan_list(ctx, plan, vd->type, ast_as(ASTInitList, vd->init), 0))
                {
                    return false;
                }
                vd->plan = plan;
            }
            else if (vd->init->kind == AST_STRING_LITERAL && type_is_array(vd->type))
            {
                if (!plan_char_array_from_string(ctx, vd))
                {
                    return false;
                }
            }
            else if (vd->init->kind == AST_UNARY_EXPR)
            {
                if (!plan_ptr_initializer(ctx, vd))
                {
                    return false;
                }
            }
            else if (!type_is_ptr(vd->type) || type_deref(vd->type)->kind != TYPE_CHAR)
            {
                sem_error(vd->base.loc, "string-literal initializer requires a 'char *' variable");
                ctx->error = true;
                return false;
            }
        }
        if (vd->has_const_init && (vd->type->kind == TYPE_ARRAY || type_is_record(vd->type)))
        {
            sem_error(vd->base.loc, "aggregate '%s' must be initialized with a brace-enclosed list",
                      vd->name);
            ctx->error = true;
            return false;
        }

        /* C11 §6.9.2p2: a declaration with an initializer is a definition even
           with `extern`, so normalize it to a plain external definition. */
        if (vd->storage == SC_EXTERN && (vd->has_const_init || vd->init))
        {
            vd->storage = SC_NONE;
        }

        ASTVarDecl *existing = strmap_get(ctx->global_vars, vd->name);
        if (existing)
        {
            if ((existing->storage == SC_STATIC) != (vd->storage == SC_STATIC))
            {
                sem_error(vd->base.loc, "%s declaration of '%s' follows %s declaration",
                          vd->storage == SC_STATIC ? "static" : "non-static", vd->name,
                          existing->storage == SC_STATIC ? "static" : "non-static");
                ctx->error = true;
                return false;
            }
            /* C11 §6.7.3p8-10: compatible types must have identical qualifiers;
               `int x;` followed by `const int x;` is incompatible. */
            if (type_unqual(existing->type) == type_unqual(vd->type) &&
                type_is_const(existing->type) != type_is_const(vd->type))
            {
                sem_error(vd->base.loc, "conflicting type qualifiers in declaration of '%s'",
                          vd->name);
                ctx->error = true;
                return false;
            }
            if (existing->has_const_init && vd->has_const_init)
            {
                sem_error(vd->base.loc, "redefinition of '%s'", vd->name);
                ctx->error = true;
                return false;
            }
        }
        /* The most-defined declaration wins: a definition replaces an
           extern-only declaration; an initialized definition replaces a
           tentative one. */
        bool replace = existing == NULL;
        if (existing && existing->storage == SC_EXTERN && vd->storage != SC_EXTERN)
        {
            replace = true;
        }
        if (existing && vd->has_const_init && !existing->has_const_init)
        {
            replace = true;
        }
        if (replace)
        {
            strmap_set(ctx->global_vars, vd->name, vd);
        }
    }
    return true;
}

/* Pass 2: Collect all function definitions */
static bool collect_function_definitions(ASTProgram *prog, SemanticCtx *ctx)
{
    size_t ndecls = vec_size(prog->decls);
    for (size_t i = 0; i < ndecls; i++)
    {
        ASTNode *decl = (ASTNode *) vec_get(prog->decls, i);
        if (decl->kind == AST_STRUCT_DECL || decl->kind == AST_ENUM_DECL ||
            decl->kind == AST_VAR_DECL)
        {
            continue;
        }
        if (decl->kind == AST_TYPEDEF_DECL)
        {
            /* Validate file-scope typedefs (the block-scope form is checked on
               the statement path). */
            if (!check_typedef_decl(ast_as(ASTTypedefDecl, decl), ctx))
            {
                return false;
            }
            continue;
        }
        if (decl->kind != AST_FUNC_DEF)
        {
            sem_error(decl->loc, "expected function definition at top level");
            return false;
        }
        ASTFuncDef *fn = ast_as(ASTFuncDef, decl);
        ASTNode *prev = strmap_get(ctx->globals, fn->name);
        if (prev)
        {
            ASTFuncDef *pfn = ast_as(ASTFuncDef, prev);
            if ((pfn->storage == SC_STATIC) != (fn->storage == SC_STATIC))
            {
                sem_error(decl->loc, "%s declaration of '%s' follows %s declaration",
                          fn->storage == SC_STATIC ? "static" : "non-static", fn->name,
                          pfn->storage == SC_STATIC ? "static" : "non-static");
                return false;
            }
            sem_error(decl->loc, "redefinition of '%s'", fn->name);
            return false;
        }
        if (strmap_get(ctx->global_vars, fn->name))
        {
            sem_error(decl->loc, "redefinition of '%s'", fn->name);
            return false;
        }
        strmap_set(ctx->globals, fn->name, fn);
    }
    return true;
}

/* Pass 3: Check each function body */
static bool check_function_bodies(ASTProgram *prog, SemanticCtx *ctx)
{
    size_t ndecls = vec_size(prog->decls);
    for (size_t i = 0; i < ndecls; i++)
    {
        ASTNode *decl = (ASTNode *) vec_get(prog->decls, i);
        if (decl->kind != AST_FUNC_DEF)
        {
            continue;
        }
        if (!check_func(decl, ctx))
        {
            return false;
        }
    }
    return true;
}

ASTNode *semantic_check(ASTNode *ast, Arena *arena)
{
    if (!ast)
    {
        return NULL;
    }

    if (ast->kind != AST_PROGRAM)
    {
        sem_error(ast->loc, "expected program at top level");
        return NULL;
    }

    ASTProgram *prog = ast_as(ASTProgram, ast);
    SemanticCtx ctx = {
        .arena = arena,
        .globals = strmap_new(arena),
        .global_vars = strmap_new(arena),
        .scopes = vec_new(arena),
        .labels = NULL,
        .loop_depth = 0,
        .switch_depth = 0,
        .switch_sem_stack = vec_new(arena),
        .error = false,
    };

    if (!collect_global_variables(prog, &ctx))
    {
        return NULL;
    }

    if (!collect_function_definitions(prog, &ctx))
    {
        return NULL;
    }

    if (!check_function_bodies(prog, &ctx))
    {
        return NULL;
    }

    return ast;
}
