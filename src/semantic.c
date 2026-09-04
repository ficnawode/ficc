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
static bool plan_var_aggregate_init(SemanticCtx *ctx, ASTVarDecl *vd, bool *handled);
static InitPlan *init_plan_new(SemanticCtx *ctx, Type *obj_type);
static bool check_compound_literal(ASTCompoundLiteral *cl, SemanticCtx *ctx);

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

static bool is_compound_assign_op(BinOpKind op)
{
    return op >= BIN_ADD_ASSIGN && op <= BIN_XOR_ASSIGN;
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

/* The single modifiable-lvalue gate (steering §Phase 9): an lvalue that is
   not const. Every write-introducing operator — plain assignment, compound
   assignment (Phase 13c), and `++`/`--` (Phase 13b) — routes through this one
   check, so the const interplay (incl. typedef'd const pointers, Phase 12)
   is enforced identically everywhere. */
/* True when the lvalue expression denotes an array, including the decayed
   cases: an array identifier's expr_type is a pointer (type_decay), but its
   declared type is an array, and a member whose field_type is an array. An
   array is never a modifiable lvalue (§6.3.2.1), so the write gate must see
   through the decay to reject `a = x`, `a++`, `a += 1`. */
static bool lvalue_is_array(ASTNode *lhs)
{
    if (type_is_array(lhs->expr_type))
    {
        return true;
    }
    if (lhs->kind == AST_IDENT)
    {
        ASTIdent *id = ast_as(ASTIdent, lhs);
        return id->decl && type_is_array(id->decl->type);
    }
    if (lhs->kind == AST_MEMBER_ACCESS)
    {
        return type_is_array(ast_as(ASTMemberAccess, lhs)->field_type);
    }
    return false;
}

static bool check_modifiable_lvalue(ASTNode *lhs, SemanticCtx *ctx)
{
    bool is_lvalue = lhs->kind == AST_IDENT || lhs->kind == AST_MEMBER_ACCESS ||
                     lhs->kind == AST_SUBSCRIPT_EXPR ||
                     (lhs->kind == AST_UNARY_EXPR && ast_as(ASTUnaryExpr, lhs)->op == UN_DEREF);
    if (!is_lvalue)
    {
        sem_error(lhs->loc, "lvalue required as left operand of assignment");
        ctx->error = true;
        return false;
    }
    if (lvalue_is_array(lhs))
    {
        sem_error(lhs->loc, "array type is not a modifiable lvalue");
        ctx->error = true;
        return false;
    }
    if (type_is_const(lhs->expr_type))
    {
        sem_error(lhs->loc, "assignment to const-qualified lvalue (read-only object)");
        ctx->error = true;
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
    bool left_void_ok = binary_expr->op == BIN_COMMA; /* §6.5.17p2: the comma's
                                left operand is evaluated as a void expression */
    if ((!left_void_ok && !check_value_used(binary_expr->left, ctx)) ||
        !check_value_used(binary_expr->right, ctx))
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
        if (!check_modifiable_lvalue(lhs, ctx))
        {
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
    else if (is_compound_assign_op(binary_expr->op))
    {
        /* §6.5.16.2: `E1 op= E2` ≡ `E1 = E1 op (E2)` with E1 evaluated once.
           The lvalue gate is the plain-assignment gate (const, arrays);
           records are already rejected above; a pointer lhs is legal only for
           `+=`/`-=` with an integer rhs (`p -= q` is E1 = ptr minus ptr = an
           integer — not assignable back). */
        ASTNode *lhs = binary_expr->left;
        if (!check_modifiable_lvalue(lhs, ctx))
        {
            return false;
        }
        bool is_ptr_add_sub =
            binary_expr->op == BIN_ADD_ASSIGN || binary_expr->op == BIN_SUB_ASSIGN;
        if (type_is_ptr(lt) && (!is_ptr_add_sub || type_is_ptr(rt)))
        {
            sem_error(binary_expr->base.loc,
                      "invalid operands to compound assignment (pointer allowed only with "
                      "'+=' / '-=' and an integer operand)");
            ctx->error = true;
            return false;
        }
        if (!type_is_ptr(lt) && !type_is_integer(rt))
        {
            sem_error(binary_expr->base.loc, "invalid operands to compound assignment");
            ctx->error = true;
            return false;
        }
        result = type_rvalue(lt);
    }
    else if (binary_expr->op == BIN_COMMA)
    {
        /* §6.5.17: the left operand is evaluated and discarded; the value and
           type of the expression are the right operand's. The result is not
           an lvalue (an lvalue on the left is fine — it is simply evaluated).
           A record right operand was already rejected by the record check
           above (records are not values). */
        result = type_rvalue(rt);
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
        if (operand->kind == AST_COMPOUND_LITERAL)
        {
            /* `&(struct S){...}`: the anonymous object's address (D12.9). The
               pointee is the declared lvalue type, qualifiers included, so
               `&(const struct S){...}` is `const struct S *`. */
            Type *ty = ast_as(ASTCompoundLiteral, operand)->type;
            unary_expr->base.expr_type = type_ptr(type_decay(ty));
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

static bool check_incdec_expr(ASTIncDecExpr *incdec, SemanticCtx *ctx)
{
    if (!check_expr(incdec->operand, ctx))
    {
        return false;
    }
    ASTNode *operand = incdec->operand;
    if (!check_modifiable_lvalue(operand, ctx))
    {
        return false;
    }
    Type *t = operand->expr_type;
    if (!type_is_integer(t) && !type_is_ptr(t))
    {
        sem_error(incdec->base.loc, "invalid operand to '%s' (arithmetic or pointer type required)",
                  incdec->is_inc ? "++" : "--");
        ctx->error = true;
        return false;
    }
    /* §6.5.2.4p3/p4: the result is an rvalue of the operand's type (never an
       lvalue). */
    incdec->base.expr_type = type_rvalue(t);
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
    if (callee->is_variadic)
    {
        /* §6.5.2.2p6: the fixed (named) part is enforced; extra args are
           legal. */
        if (got < expected)
        {
            sem_error(call_expr->base.loc, "function '%s' expects at least %zu arguments, got %zu",
                      call_expr->callee, expected, got);
            ctx->error = true;
            return false;
        }
    }
    else if (expected != got)
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
        /* Only the named parameters have a declared type to check against;
           the variadic tail is assignability-free here (default promotions
           land in the IR builder). */
        if (i >= expected)
        {
            continue;
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
        case AST_INCDEC_EXPR:
            if (!check_incdec_expr(ast_as(ASTIncDecExpr, node), ctx))
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
            else if (se->operand->kind == AST_COMPOUND_LITERAL &&
                     type_is_array(ast_as(ASTCompoundLiteral, se->operand)->type))
            {
                /* `sizeof((int[]){...})`: a compound literal does not decay
                   either (§6.5.2.5p4 note) — the array type survives. */
                op_type = ast_as(ASTCompoundLiteral, se->operand)->type;
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
        case AST_ALIGNOF_EXPR:
        {
            ASTAlignofExpr *ae = ast_as(ASTAlignofExpr, node);
            if (!check_expr(ae->operand, ctx))
            {
                return NULL;
            }
            Type *op_type = ae->operand->expr_type;
            /* §6.3.2.1p3: array-to-pointer decay is suppressed for the direct
               operand of _Alignof, so `_Alignof(arr)` is the element
               alignment (arrays align as their element type). */
            if (ae->operand->kind == AST_IDENT)
            {
                ASTVarDecl *decl = ast_as(ASTIdent, ae->operand)->decl;
                if (decl && type_is_array(decl->type))
                {
                    op_type = decl->type;
                }
            }
            /* §6.5.3.4p2 constraint: the operand type shall not be a function
               type or an incomplete type. A void or array-of-void lvalue is
               already caught by check_expr; functions don't exist as rvalues
               here. */
            if (op_type->kind == TYPE_VOID)
            {
                sem_error(node->loc, "_Alignof(void) is invalid");
                ctx->error = true;
                return NULL;
            }
            if (!type_is_complete(op_type))
            {
                sem_error(node->loc, "_Alignof of incomplete type");
                ctx->error = true;
                return NULL;
            }
            ae->align_value = type_alignof(op_type);
            node->expr_type = type_ulong();
            return node->expr_type;
        }
        case AST_ALIGNOF_TYPE:
        {
            ASTAlignofType *at = ast_as(ASTAlignofType, node);
            if (at->type->kind == TYPE_VOID)
            {
                sem_error(node->loc, "_Alignof(void) is invalid");
                ctx->error = true;
                return NULL;
            }
            if (!type_is_complete(at->type))
            {
                sem_error(node->loc, "_Alignof of incomplete type");
                ctx->error = true;
                return NULL;
            }
            at->align_value = type_alignof(at->type);
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
        case AST_COMPOUND_LITERAL:
            if (!check_compound_literal(ast_as(ASTCompoundLiteral, node), ctx))
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
           constant initializer into const_init, routed a brace list, a string
           literal, or an address constant into init; everything else is
           rejected. Lists / char arrays get a plan; the serializer emits the
           bytes. */
        if (var_decl->init)
        {
            bool handled;
            if (!plan_var_aggregate_init(ctx, var_decl, &handled))
            {
                return false;
            }
            if (!handled)
            {
                if (var_decl->init->kind == AST_UNARY_EXPR)
                {
                    if (!plan_ptr_initializer(ctx, var_decl))
                    {
                        return false;
                    }
                }
                else if (!type_is_ptr(var_decl->type) ||
                         type_deref(var_decl->type)->kind != TYPE_CHAR)
                {
                    sem_error(var_decl->base.loc,
                              "string-literal initializer requires a 'char *' variable");
                    ctx->error = true;
                    return false;
                }
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
        bool handled;
        if (!plan_var_aggregate_init(ctx, var_decl, &handled))
        {
            return false;
        }
        if (handled)
        {
            return true;
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
    /* D12.7: a `[]` array with no initializer (or an incomplete record/array
       element) stays incomplete — rejected after completion would have run. */
    if (!type_is_complete(var_decl->type))
    {
        sem_error(var_decl->base.loc, "variable '%s' has incomplete type", var_decl->name);
        ctx->error = true;
        return false;
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
    bool grow; /* root `[]` array: unbounded cursor, tracked (D12.7) */
} PlanFrame;

static bool is_aggregate_type(Type *t)
{
    t = type_unqual(t);
    return type_is_array(t) || type_is_record(t);
}

static u32 aggr_nchildren(const PlanFrame *fr)
{
    Type *agg = type_unqual(fr->agg);
    if (type_is_array(agg))
    {
        if (fr->grow)
        {
            /* Declared-against `[]` array: never pops by exhaustion. */
            return UINT32_MAX;
        }
        return (u32) agg->arr.length;
    }
    return (u32) vec_size(agg->record.fields);
}

static bool aggr_child(const PlanFrame *fr, Type **cty, u32 *coff)
{
    Type *agg = type_unqual(fr->agg);
    u32 idx = fr->next;
    u32 base = fr->base;
    if (type_is_array(agg))
    {
        if (!fr->grow && idx >= agg->arr.length)
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
        if (top->next < aggr_nchildren(top))
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
    plan->grow_array = false;
    plan->inferred_len = 0;
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

static bool resolve_designator_path(SemanticCtx *ctx, InitPlan *plan, Type *t, u32 base_off,
                                    Designator *d, Vec *path, Type **out_ty, u32 *out_off, Loc loc)
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
            /* Only the outermost declared-against `[]` array may take an
               out-of-range designator (it sizes the array, D12.7). Inner
               brackets stay bounded. */
            bool grow = plan->grow_array && cur_ty->arr.length == 0;
            if (dd->index < 0 || (!grow && (u64) dd->index >= cur_ty->arr.length))
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
            fr->grow = grow;
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
                fr->grow = false;
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
        if (sl->length > cty->arr.length)
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

    /* §6.7.9p14: a character array may be initialized by a character string
       literal, braced or not — `char s[5] = "hi"` and `char s[5] = {"hi"}`
       both fill the array; an unsized target infers strlen+1 from the string.
       The terminating NUL is stored *if there is room*: `char s[2] = "hi"`
       drops it (2 chars, no room), `char s[3] = "hi"` keeps it. The only
       error is `strlen > size` (`char s[1] = "hi"`). The emission clamps the
       copy length to the array size (ir_builder) so the NUL is dropped there. */
    if (type_is_array(t) && type_array_elem(t)->kind == TYPE_CHAR && nel == 1)
    {
        InitElem *e = (InitElem *) vec_get(list->elems, 0);
        if (!e->design && e->value->kind == AST_STRING_LITERAL)
        {
            ASTStringLiteral *sl = ast_as(ASTStringLiteral, e->value);
            u64 need = sl->length + 1;
            if (t->arr.length == 0)
            {
                if (!plan->grow_array)
                {
                    sem_error(list->base.loc, "array has incomplete type");
                    ctx->error = true;
                    return false;
                }
                plan->inferred_len = need;
                /* The write's type is used only for the copy length (strlen+1),
                   so recording it against the yet-to-be-completed type is safe:
                   completion in plan_brace_list resizes the object afterwards. */
                plan_new_write(ctx, plan, base_off, t, e->value, true);
                return true;
            }
            if (sl->length > t->arr.length)
            {
                sem_error(e->loc, "initializer-string for array of chars is too long");
                ctx->error = true;
                return false;
            }
            plan_new_write(ctx, plan, base_off, t, e->value, true);
            return true;
        }
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
        /* A `[]` rank may only be the outermost, declared-against target:
           it grows from its initializer (D12.7). Inner empty brackets error. */
        if (!plan->grow_array)
        {
            sem_error(list->base.loc, "array has incomplete type");
            ctx->error = true;
            return false;
        }
    }

    Vec *stack = vec_new(ctx->arena);
    PlanFrame *root = arena_alloc(ctx->arena, sizeof(PlanFrame), _Alignof(PlanFrame));
    root->agg = t;
    root->next = 0;
    root->base = base_off;
    root->grow = plan->grow_array && type_is_array(t) && t->arr.length == 0;
    vec_push(stack, root);

    for (size_t i = 0; i < nel; i++)
    {
        InitElem *e = (InitElem *) vec_get(list->elems, i);

        if (e->design)
        {
            Vec *path = vec_new(ctx->arena);
            Type *dt;
            u32 doff;
            if (!resolve_designator_path(ctx, plan, t, base_off, e->design, path, &dt, &doff,
                                         e->loc))
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
        if (!aggr_child(top, &cty, &coff))
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
                fr->grow = false;
                vec_push(stack, fr);
                top = (PlanFrame *) vec_last(stack);
                if (!aggr_child(top, &cty, &coff))
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

        /* The growable root's cursor (after the clause) is the inferred length: a
           boundary position (`root.next`) counts fully-consumed elements; a
           cursor still inside the current element — a deeper frame remains on
           the stack — counts `root.next + 1` (a partially-filled row is one
           element, §6.7.9p22 / D12.7). */
        if (plan->grow_array && vec_size(stack) > 0)
        {
            PlanFrame *rf = (PlanFrame *) vec_get(stack, 0);
            if (rf->grow)
            {
                u64 count = rf->next + (vec_size(stack) > 1 ? 1 : 0);
                if (count > plan->inferred_len)
                {
                    plan->inferred_len = count;
                }
            }
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
    if (sl->length > type_array_len(arr))
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

/* `int *p = &g;` or `int *p = &(type){...};` (block static or file scope): a
   bare address constant in pointer position. The value type-checks against
   the pointer target; the IR serializer turns it into one 8-byte relocation
   write (§6.6p9, D12.8/D12.9 — a compound literal is an address constant). */
static bool plan_ptr_initializer(SemanticCtx *ctx, ASTVarDecl *vd)
{
    if (vd->init->kind != AST_UNARY_EXPR)
    {
        return false;
    }
    ASTUnaryExpr *u = ast_as(ASTUnaryExpr, vd->init);
    if (u->op != UN_ADDR ||
        (u->operand->kind != AST_IDENT && u->operand->kind != AST_COMPOUND_LITERAL))
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

/* Run the planner over a brace-list initializer, completing a declared-against
   `[]` array *through* `type_out` first (D12.7 — the fresh type is built from
   the planner's inferred length; the interned 0-length type is never mutated,
   P3). Shared by var declarations and compound literals (D12.9). Returns false
   on error. */
static bool plan_brace_list(SemanticCtx *ctx, Type **type_out, ASTNode *init, InitPlan **plan_out)
{
    InitPlan *plan = init_plan_new(ctx, *type_out);
    bool grow = type_is_array(*type_out) && type_array_len(*type_out) == 0;
    plan->grow_array = grow;
    if (!plan_list(ctx, plan, *type_out, ast_as(ASTInitList, init), 0))
    {
        return false;
    }
    if (grow)
    {
        Type *elem = type_array_elem(*type_out);
        *type_out = type_array(elem, plan->inferred_len);
        plan->total_size = type_sizeof(*type_out);
    }
    *plan_out = plan;
    return true;
}

/* Build the initializer plan for a brace list or char-array string, completing
   a declared-against `[]` array first (D12.7). `*handled` is set when
   `vd->init` matched one of these forms. Returns false on error. */
static bool plan_var_aggregate_init(SemanticCtx *ctx, ASTVarDecl *vd, bool *handled)
{
    *handled = false;
    if (vd->init->kind == AST_INIT_LIST)
    {
        *handled = true;
        return plan_brace_list(ctx, &vd->type, vd->init, &vd->plan);
    }
    if (vd->init->kind == AST_STRING_LITERAL && type_is_array(vd->type))
    {
        *handled = true;
        if (type_array_len(vd->type) == 0)
        {
            ASTStringLiteral *sl = ast_as(ASTStringLiteral, vd->init);
            vd->type = type_array(type_array_elem(vd->type), sl->length + 1);
        }
        return plan_char_array_from_string(ctx, vd);
    }
    return true;
}

/* A compound literal `(type){ ... }` (C11 §6.5.2.5, D12.9). Check the
   target type, run the planner against the list (completing `[]`), and set the
   node's lowering plan. The node is an lvalue whose type is the *declared*
   type (qualifiers intact: `(const struct S){...}` is a const lvalue);
   initialization itself bypasses the §9 write gate. */
static bool check_compound_literal(ASTCompoundLiteral *cl, SemanticCtx *ctx)
{
    Type *ty = type_unqual(cl->type);
    if (ty->kind == TYPE_VOID)
    {
        sem_error(cl->base.loc, "conversion to non-scalar type requested");
        ctx->error = true;
        return false;
    }
    if (type_is_record(ty) && !type_is_complete(ty))
    {
        sem_error(cl->base.loc, "compound literal of incomplete type");
        ctx->error = true;
        return false;
    }
    if (!plan_brace_list(ctx, &cl->type, cl->init, &cl->plan))
    {
        return false;
    }
    cl->base.expr_type = type_decay(cl->type);
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
        case AST_ALIGNOF_TYPE:
            /* check_expr has already computed align_value. */
            *out = (i64) ast_as(ASTAlignofType, node)->align_value;
            return true;
        case AST_ALIGNOF_EXPR:
            /* check_expr has already resolved the operand type and set
               align_value, so `case _Alignof(x):` works here. */
            *out = (i64) ast_as(ASTAlignofExpr, node)->align_value;
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

/* `_Static_assert(expr, "msg")` (§6.7.4): expr must be an integer constant
   expression; the value is checked at compile time and the message reported on
   failure. check_expr runs first so type-dependent subexpressions
   (`sizeof(x)`, `_Alignof(x)`) resolve, then fold_integer_constant evaluates. */
static bool check_static_assert(ASTStaticAssert *sa, SemanticCtx *ctx)
{
    if (!check_expr(sa->expr, ctx))
    {
        ctx->error = true;
        return false;
    }
    i64 value;
    if (!fold_integer_constant(sa->expr, &value))
    {
        sem_error(sa->base.loc,
                  "static assertion expression is not an integer constant expression");
        ctx->error = true;
        return false;
    }
    if (value == 0)
    {
        sem_error(sa->base.loc, "static assertion failed: %s", sa->msg);
        ctx->error = true;
        return false;
    }
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
        case AST_DECL_LIST:
        {
            /* An init-declarator list: each declarator is an independent
               declaration sharing the specifier's type. */
            ASTDeclList *dl = ast_as(ASTDeclList, node);
            size_t n = vec_size(dl->decls);
            for (size_t i = 0; i < n; i++)
            {
                if (!check_variable_declaration(
                        ast_as(ASTVarDecl, (ASTNode *) vec_get(dl->decls, i)), ctx))
                {
                    return false;
                }
            }
            return true;
        }
        case AST_STRUCT_DECL:
        case AST_ENUM_DECL:
            /* A block-scope tag definition: the parser completed the type at
               parse time; nothing to check or emit. */
            return true;
        case AST_TYPEDEF_DECL:
            return check_typedef_decl(ast_as(ASTTypedefDecl, node), ctx);
        case AST_STATIC_ASSERT:
            return check_static_assert(ast_as(ASTStaticAssert, node), ctx);
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

/* The interned function type of a definition (D15.1): the return type and the
   parameter types unqualified — top-level qualifiers are ignored for
   function-type compatibility (§6.7.6.3p15) — plus the variadic bit. */
static Type *build_func_type(ASTFuncDef *fn, SemanticCtx *ctx)
{
    Vec *param_types = vec_new(ctx->arena);
    size_t nparams = vec_size(fn->params);
    for (size_t i = 0; i < nparams; i++)
    {
        ASTVarDecl *param = ast_as(ASTVarDecl, (ASTNode *) vec_get(fn->params, i));
        vec_push(param_types, type_unqual(param->type));
    }
    return type_func(type_unqual(fn->ret_type), param_types, fn->is_variadic);
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
    fn->func_type = build_func_type(fn, ctx);

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
/* Validate and register one file-scope variable declaration. The most-
   defined declaration wins: a definition replaces an extern-only
   declaration; an initialized definition replaces a tentative one. */
static bool collect_one_global_var(ASTVarDecl *vd, SemanticCtx *ctx)
{
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
        /* Initializer lists are flattened by the planner (completing a
           `[]` array per D12.7); char arrays take a string literal
           byte-fill; bare `char *` pointers keep the .data string-address
           relocation (D12.6/D12.8). */
        bool handled;
        if (!plan_var_aggregate_init(ctx, vd, &handled))
        {
            return false;
        }
        if (!handled)
        {
            if (vd->init->kind == AST_UNARY_EXPR)
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
    }
    if (vd->has_const_init && (vd->type->kind == TYPE_ARRAY || type_is_record(vd->type)))
    {
        sem_error(vd->base.loc, "aggregate '%s' must be initialized with a brace-enclosed list",
                  vd->name);
        ctx->error = true;
        return false;
    }
    /* D12.7: a file-scope `[]` array left without an initializer stays
       incomplete. `extern int a[];` declares (not defines) an incomplete
       array and is legal. */
    if (vd->storage != SC_EXTERN && type_is_array(vd->type) && !type_is_complete(vd->type))
    {
        sem_error(vd->base.loc, "variable '%s' has incomplete type", vd->name);
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
            sem_error(vd->base.loc, "conflicting type qualifiers in declaration of '%s'", vd->name);
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
    return true;
}

static bool collect_global_variables(ASTProgram *prog, SemanticCtx *ctx)
{
    size_t ndecls = vec_size(prog->decls);
    for (size_t i = 0; i < ndecls; i++)
    {
        ASTNode *decl = (ASTNode *) vec_get(prog->decls, i);
        if (decl->kind == AST_VAR_DECL)
        {
            if (!collect_one_global_var(ast_as(ASTVarDecl, decl), ctx))
            {
                return false;
            }
        }
        else if (decl->kind == AST_DECL_LIST)
        {
            ASTDeclList *dl = ast_as(ASTDeclList, decl);
            size_t n = vec_size(dl->decls);
            for (size_t j = 0; j < n; j++)
            {
                if (!collect_one_global_var(ast_as(ASTVarDecl, (ASTNode *) vec_get(dl->decls, j)),
                                            ctx))
                {
                    return false;
                }
            }
        }
    }
    return true;
}

/* Pass 0: check file-scope `_Static_assert`s before anything else. */
static bool check_file_scope_asserts(ASTProgram *prog, SemanticCtx *ctx)
{
    size_t ndecls = vec_size(prog->decls);
    for (size_t i = 0; i < ndecls; i++)
    {
        ASTNode *decl = (ASTNode *) vec_get(prog->decls, i);
        if (decl->kind != AST_STATIC_ASSERT)
        {
            continue;
        }
        if (!check_static_assert(ast_as(ASTStaticAssert, decl), ctx))
        {
            return false;
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
            decl->kind == AST_VAR_DECL || decl->kind == AST_DECL_LIST ||
            decl->kind == AST_STATIC_ASSERT)
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

    if (!check_file_scope_asserts(prog, &ctx))
    {
        return NULL;
    }

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
