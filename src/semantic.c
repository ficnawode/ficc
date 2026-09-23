#include "semantic.h"
#include "util/assert.h"
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
    Vec *fn_params;        /* enclosing function's ASTVarDecl* list (for builtin
                              va_start validation), NULL outside function bodies */
    SemanticConfig cfg;
    bool error;
};

typedef struct SwitchSem SwitchSem;
struct SwitchSem
{
    U64Map *values;      /* (u64)converted case value -> non-NULL, for duplicate detection */
    Type *promoted_cond; /* type_promote(controlling expression type) */
    bool has_default;
};

typedef enum
{
    PLAN_HANDLED, /* aggregate / string / address-constant initializer was planned */
    PLAN_NONE,    /* scalar initializer — not planned; the caller checks it as an expression */
    PLAN_ERROR,   /* a diagnostic was emitted; the caller must abort */
} PlanResult;

static Type *check_expr(ASTNode *node, SemanticCtx *ctx);
static bool check_stmt(ASTNode *node, SemanticCtx *ctx, Type *ret_type);
static bool check_func(ASTNode *node, SemanticCtx *ctx);
static bool check_ternary_expression(ASTTernaryExpr *ternary, SemanticCtx *ctx);
static bool check_cast_expr(ASTCastExpr *ce, SemanticCtx *ctx);
static bool plan_list(SemanticCtx *ctx, InitPlan *plan, Type *t, ASTInitList *list, u32 base_off);
static bool plan_var_aggregate_init(SemanticCtx *ctx, ASTVarDecl *vd, bool *handled);
static bool check_aggregate_const_init(ASTVarDecl *vd, SemanticCtx *ctx);
static PlanResult plan_var_initializer(SemanticCtx *ctx, ASTVarDecl *vd);
static InitPlan *init_plan_new(SemanticCtx *ctx, Type *obj_type);
static bool check_compound_literal(ASTCompoundLiteral *cl, SemanticCtx *ctx);
static bool check_generic_selection(ASTGenericSelection *gs, SemanticCtx *ctx);
static bool fold_integer_constant(ASTNode *node, i64 *out);
static bool sem_resolve_type(Type **slot, SemanticCtx *ctx);

static bool sem_error(SemanticCtx *ctx, Loc loc, const char *fmt, ...)
{
    fprintf(stderr, "%s:%u:%u: [semantic] error: ", loc.file, loc.line, loc.col);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
    ctx->error = true;
    return false;
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
        return sem_error(ctx, node->loc, "void value not ignored as it ought to be");
    }
    return true;
}

/* A condition operand; floats are admitted now — the IR builder boolifies them
   with FCMP_NE(x, 0.0), so -0.0/NaN never bit-test. */
static bool check_condition(SemanticCtx *ctx, ASTNode *cond)
{
    return check_expr(cond, ctx) && check_value_used(cond, ctx);
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

static ASTVarDecl *scope_top_lookup(SemanticCtx *ctx, const char *name)
{
    return strmap_get(current_scope(ctx), name);
}

/* Both AST_FUNC_DEF and AST_FUNC_DECL embed a FuncSig; either kind reduces to it. */
static FuncSig *func_sig_of(ASTNode *node)
{
    if (node->kind == AST_FUNC_DEF)
    {
        return &ast_as(ASTFuncDef, node)->sig;
    }
    return &ast_as(ASTFuncDecl, node)->sig;
}

static bool func_node_defined(ASTNode *node)
{
    return node->kind == AST_FUNC_DEF;
}

static bool check_identifier_expr(ASTIdent *ident, SemanticCtx *ctx)
{
    ASTVarDecl *decl = scope_lookup(ctx, ident->name);
    if (!decl)
    {
        decl = strmap_get(ctx->global_vars, ident->name);
    }
    if (decl)
    {
        ident->decl = decl;
        ident->base.expr_type = type_decay(decl->type);
        return true;
    }
    /* A function designator (§6.3.2.1p4): its value is a pointer to the function. */
    ASTNode *fnode = strmap_get(ctx->globals, ident->name);
    if (fnode)
    {
        ident->is_func = true;
        ident->base.expr_type = type_ptr(func_sig_of(fnode)->func_type);
        return true;
    }
    return sem_error(ctx, ident->base.loc, "undeclared identifier '%s'", ident->name);
}

static const bool is_comparison_op_table[BIN_GE + 1] = {
    [BIN_EQ] = true, [BIN_NE] = true, [BIN_LT] = true,
    [BIN_GT] = true, [BIN_LE] = true, [BIN_GE] = true,
};

static const bool is_compound_assign_table[BIN_XOR_ASSIGN + 1] = {
    [BIN_ADD_ASSIGN] = true, [BIN_SUB_ASSIGN] = true, [BIN_MUL_ASSIGN] = true,
    [BIN_DIV_ASSIGN] = true, [BIN_REM_ASSIGN] = true, [BIN_SHL_ASSIGN] = true,
    [BIN_SHR_ASSIGN] = true, [BIN_AND_ASSIGN] = true, [BIN_OR_ASSIGN] = true,
    [BIN_XOR_ASSIGN] = true,
};

static bool is_comparison_op(BinOpKind op)
{
    return op <= BIN_GE && is_comparison_op_table[op];
}

static bool is_compound_assign_op(BinOpKind op)
{
    return op <= BIN_XOR_ASSIGN && is_compound_assign_table[op];
}

/* Operators that are integer-only (§6.5.5/% , §6.5.7 shifts, §6.5.12-14
   bitwise): a floating operand is a constraint violation in every case,
   including their compound-assignment forms. */
static bool op_requires_integer(BinOpKind op)
{
    switch (op)
    {
        case BIN_REM:
        case BIN_AND:
        case BIN_OR:
        case BIN_XOR:
        case BIN_SHL:
        case BIN_SHR:
        case BIN_REM_ASSIGN:
        case BIN_AND_ASSIGN:
        case BIN_OR_ASSIGN:
        case BIN_XOR_ASSIGN:
        case BIN_SHL_ASSIGN:
        case BIN_SHR_ASSIGN:
            return true;
        default:
            return false;
    }
}

/* §6.5.16.1p1 assignment compatibility (used by `=`, call args, returns,
   initializers): pointers gain qualifiers only at the first pointee level
   (`int**` is not assignable to `const int**`); records must be identical;
   other scalars convert freely. */
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

/* True when the (possibly decayed) lvalue expression denotes an array: arrays
   are never modifiable lvalues (§6.3.2.1), so the write gate must see through
   the decay to reject `a = x`, `a++`, `a += 1`. */
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
        return sem_error(ctx, lhs->loc, "lvalue required as left operand of assignment");
    }
    if (lvalue_is_array(lhs))
    {
        return sem_error(ctx, lhs->loc, "array type is not a modifiable lvalue");
    }
    if (type_is_const(lhs->expr_type))
    {
        return sem_error(ctx, lhs->loc, "assignment to const-qualified lvalue (read-only object)");
    }
    return true;
}

/* §6.5.16.1: `E1 = E2` requires a modifiable lvalue; pointer and record targets
   are checked for assignability. Returns the result type (an rvalue of the lhs
   type), or NULL after emitting an error. */
static Type *check_assign_expr(ASTBinaryExpr *be, Type *lt, Type *rt, SemanticCtx *ctx)
{
    if (!check_modifiable_lvalue(be->left, ctx))
    {
        return NULL;
    }
    if (type_is_ptr(lt) && type_is_ptr(rt) && !type_assignable(lt, rt))
    {
        sem_error(ctx, be->base.loc, "incompatible pointer types in assignment");
        return NULL;
    }
    if ((type_is_record(lt) || type_is_record(rt)) && !type_assignable(lt, rt))
    {
        sem_error(ctx, be->base.loc, "incompatible types in struct/union assignment");
        return NULL;
    }
    return type_rvalue(lt);
}

/* §6.5.16.2: `E1 op= E2` = `E1 = E1 op (E2)` with E1 evaluated once; a pointer
   lhs is legal only for `+=`/`-=` with an integer rhs. */
static Type *check_compound_assign_expr(ASTBinaryExpr *be, Type *lt, Type *rt, SemanticCtx *ctx)
{
    if (!check_modifiable_lvalue(be->left, ctx))
    {
        return NULL;
    }
    bool is_ptr_add_sub = be->op == BIN_ADD_ASSIGN || be->op == BIN_SUB_ASSIGN;
    if (type_is_ptr(lt) && (!is_ptr_add_sub || type_is_ptr(rt)))
    {
        sem_error(ctx, be->base.loc,
                  "invalid operands to compound assignment (pointer allowed only with "
                  "'+=' / '-=' and an integer operand)");
        return NULL;
    }
    if (!type_is_ptr(lt) && !type_is_integer(rt) && !type_is_fp(rt))
    {
        sem_error(ctx, be->base.loc, "invalid operands to compound assignment");
        return NULL;
    }
    return type_rvalue(lt);
}

/* Result type of a non-assignment binary op on scalar/pointer operands: logical
   and comparison ops yield int; pointer add/sub of a pointer and an integer
   keeps the pointer; otherwise the usual arithmetic conversion's common type. */
static Type *value_op_result(BinOpKind op, Type *lt, Type *rt)
{
    if (op == BIN_LOG_AND || op == BIN_LOG_OR || is_comparison_op(op))
    {
        return type_int();
    }
    if (type_is_ptr(lt) && type_is_ptr(rt))
    {
        /* §6.5.6p9: subtracting two pointers yields ptrdiff_t (long on LP64).
           Every other pointer-`op`-pointer arithmetic is ill-formed. */
        return op == BIN_SUB ? type_long() : NULL;
    }
    if (type_is_ptr(lt) && (op == BIN_ADD || op == BIN_SUB) && !type_is_ptr(rt))
    {
        return lt;
    }
    if (type_is_ptr(rt) && op == BIN_ADD && !type_is_ptr(lt))
    {
        /* §6.5.6p7: pointer + integer (and the commutative integer + pointer). */
        return rt;
    }
    if (type_is_ptr(lt) || type_is_ptr(rt))
    {
        return NULL;
    }
    /* §6.5.7p3: a shift's result type is the promoted left operand. */
    if (op == BIN_SHL || op == BIN_SHR)
    {
        return type_promote(lt);
    }
    return type_common(type_promote(lt), type_promote(rt));
}

static bool check_binary_expr(ASTBinaryExpr *binary_expr, SemanticCtx *ctx)
{
    if (!check_expr(binary_expr->left, ctx) || !check_expr(binary_expr->right, ctx))
    {
        return false;
    }
    /* Array and function designators decay to pointers in value positions
       (§6.3.2.1p3/p4); identifiers already decay in check_identifier_expr,
       this folds nested designators like `(*fp)` in `fp == *fp`. */
    Type *lt = type_decay(binary_expr->left->expr_type);
    Type *rt = type_decay(binary_expr->right->expr_type);
    bool left_void_ok = binary_expr->op == BIN_COMMA; /* §6.5.17p2: the comma's left
                                        operand is evaluated as a void expression */
    if (binary_expr->op == BIN_COMMA)
    {
        /* §6.5.17p3: the comma's result is the right operand's value and type. */
        left_void_ok = true;
    }
    else if ((!left_void_ok && !check_value_used(binary_expr->left, ctx)) ||
             !check_value_used(binary_expr->right, ctx))
    {
        /* `(void)x + 1`, `f() = 5` — operand is void, not a value. */
        return false;
    }
    if (binary_expr->op != BIN_ASSIGN && binary_expr->op != BIN_COMMA &&
        (type_is_record(lt) || type_is_record(rt)))
    {
        return sem_error(ctx, binary_expr->base.loc, "invalid operands to operator (record type)");
    }
    if (binary_expr->op != BIN_ASSIGN && binary_expr->op != BIN_COMMA &&
        (type_is_fp(lt) || type_is_fp(rt)))
    {
        /* FP arithmetic/comparisons/logical lower as float; %/shifts/bitwise
           are integer-only and a float mix is a plain constraint violation. */
        if (op_requires_integer(binary_expr->op))
        {
            return sem_error(ctx, binary_expr->base.loc,
                             "invalid operands to operator (floating-point operands)");
        }
        if ((type_is_fp(lt) && type_is_ptr(rt)) || (type_is_ptr(lt) && type_is_fp(rt)))
        {
            return sem_error(ctx, binary_expr->base.loc,
                             "invalid operands to operator (floating-point and pointer)");
        }
    }
    Type *result;
    BinOpKind op = binary_expr->op;
    if (op == BIN_ASSIGN)
    {
        result = check_assign_expr(binary_expr, lt, rt, ctx);
    }
    else if (is_compound_assign_op(op))
    {
        result = check_compound_assign_expr(binary_expr, lt, rt, ctx);
    }
    else if (op == BIN_COMMA)
    {
        /* §6.5.17: the value and type are the right operand's. */
        result = type_rvalue(rt);
    }
    else
    {
        result = value_op_result(op, type_rvalue(lt), type_rvalue(rt));
        if (!result)
        {
            return sem_error(ctx, binary_expr->base.loc, "invalid operands to operator");
        }
    }
    if (!result)
    {
        return false;
    }
    binary_expr->base.expr_type = result;
    return true;
}

/* `&operand`: a pointer to the operand's declared (lvalue) type, qualifiers
   included; NULL after emitting an error. */
static Type *check_address_of(ASTNode *operand, SemanticCtx *ctx)
{
    if (operand->kind == AST_UNARY_EXPR && ast_as(ASTUnaryExpr, operand)->op == UN_DEREF)
    {
        return type_ptr(operand->expr_type);
    }
    if (operand->kind == AST_MEMBER_ACCESS)
    {
        ASTMemberAccess *ma = ast_as(ASTMemberAccess, operand);
        if (ma->is_bitfield)
        {
            sem_error(ctx, operand->loc, "cannot take address of bit-field");
            return NULL;
        }
        /* §6.5.3.2p3: no decay, so `&s.arr` is a pointer to the array. */
        return type_ptr(ma->field_type);
    }
    if (operand->kind == AST_SUBSCRIPT_EXPR)
    {
        /* §6.5.3.2p3: no decay on the lvalue, so `&m[i]` points at the element. */
        ASTSubscriptExpr *se = ast_as(ASTSubscriptExpr, operand);
        Type *ptr_type = type_decay(se->array->expr_type);
        return type_ptr(type_deref(ptr_type));
    }
    if (operand->kind == AST_IDENT)
    {
        ASTIdent *id = ast_as(ASTIdent, operand);
        if (id->is_func)
        {
            return operand->expr_type; /* `&f` is already the function's address */
        }
        if (!id->decl)
        {
            sem_error(ctx, operand->loc, "cannot take address of this expression");
            return NULL;
        }
        return type_ptr(id->decl->type);
    }
    if (operand->kind == AST_COMPOUND_LITERAL)
    {
        /* The anonymous object's address; pointee qualifiers survive, so
           `&(const struct S){...}` is `const struct S *`. */
        Type *ty = ast_as(ASTCompoundLiteral, operand)->type;
        return type_ptr(ty);
    }
    sem_error(ctx, operand->loc, "cannot take address of this expression");
    return NULL;
}

static bool check_unary_expr(ASTUnaryExpr *unary_expr, SemanticCtx *ctx)
{
    if (!check_expr(unary_expr->operand, ctx))
    {
        return false;
    }
    Type *op_type = unary_expr->operand->expr_type;
    if (unary_expr->op == UN_BIT_NOT && type_is_fp(type_rvalue(op_type)))
    {
        /* §6.5.3.3p4: `~` is integer-only; FP floor is not `~`. */
        return sem_error(ctx, unary_expr->base.loc,
                         "invalid operand to '~' (integer type required)");
    }
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
            return sem_error(ctx, unary_expr->base.loc, "cannot dereference non-pointer type");
        }
        /* The pointee type carries the const (const int* -> const int). */
        unary_expr->base.expr_type = type_deref(op_type);
        return true;
    }
    if (unary_expr->op == UN_ADDR)
    {
        Type *t = check_address_of(unary_expr->operand, ctx);
        if (!t)
        {
            return false;
        }
        unary_expr->base.expr_type = t;
        return true;
    }
    if (op_type->kind == TYPE_VOID)
    {
        return sem_error(ctx, unary_expr->base.loc, "void value not ignored as it ought to be");
    }
    if (type_is_record(op_type))
    {
        return sem_error(ctx, unary_expr->base.loc,
                         "invalid operand of record type to unary operator");
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
    if (!type_is_integer(t) && !type_is_ptr(t) && !type_is_fp(t))
    {
        return sem_error(ctx, incdec->base.loc,
                         "invalid operand to '%s' (arithmetic or pointer type required)",
                         incdec->is_inc ? "++" : "--");
    }
    /* §6.5.2.4p3/p4: the result is an rvalue of the operand's type (never an
       lvalue). */
    incdec->base.expr_type = type_rvalue(t);
    return true;
}

/* A va_start/va_end/va_arg argument must be a `va_list` after decay: a pointer
   to the builtin va_list element type (the array-of-1 decays to the 24-byte
   struct); pointer-equality on the interned element. */
static bool builtin_check_va_list_arg(ASTNode *arg, SemanticCtx *ctx)
{
    if (!check_expr(arg, ctx))
    {
        return false;
    }
    Type *t = type_decay(arg->expr_type);
    if (!type_is_ptr(t) || type_deref(t) != type_array_elem(type_va_list()))
    {
        return sem_error(ctx, arg->loc, "argument must be a __builtin_va_list");
    }
    return true;
}

/* The va_start/va_end builtins have fixed signatures over the va_list object.
   `__builtin_va_start(ap, last)` requires `last` to name one of the enclosing
   function's parameters (its value is unused; offsets are compile-time). */
static bool check_va_builtin(ASTCallExpr *call_expr, SemanticCtx *ctx)
{
    size_t got = vec_size(call_expr->args);
    if (strcmp(call_expr->callee, "__builtin_va_start") == 0)
    {
        if (got != 2)
        {
            return sem_error(ctx, call_expr->base.loc,
                             "'__builtin_va_start' expects 2 arguments, got %zu", got);
        }
        if (!builtin_check_va_list_arg((ASTNode *) vec_get(call_expr->args, 0), ctx))
        {
            return false;
        }
        ASTNode *last = (ASTNode *) vec_get(call_expr->args, 1);
        if (!check_expr(last, ctx))
        {
            return false;
        }
        bool is_param = false;
        if (last->kind == AST_IDENT && ctx->fn_params)
        {
            ASTVarDecl *decl = ast_as(ASTIdent, last)->decl;
            size_t n = vec_size(ctx->fn_params);
            for (size_t i = 0; i < n; i++)
            {
                if ((ASTVarDecl *) vec_get(ctx->fn_params, i) == decl)
                {
                    is_param = true;
                    break;
                }
            }
        }
        if (!is_param)
        {
            return sem_error(
                ctx, last->loc,
                "'__builtin_va_start' second argument must be a parameter of the function");
        }
    }
    else if (strcmp(call_expr->callee, "__builtin_va_copy") == 0)
    {
        if (got != 2)
        {
            return sem_error(ctx, call_expr->base.loc,
                             "'__builtin_va_copy' expects 2 arguments, got %zu", got);
        }
        if (!builtin_check_va_list_arg((ASTNode *) vec_get(call_expr->args, 0), ctx) ||
            !builtin_check_va_list_arg((ASTNode *) vec_get(call_expr->args, 1), ctx))
        {
            return false;
        }
    }
    else
    {
        if (got != 1)
        {
            return sem_error(ctx, call_expr->base.loc,
                             "'__builtin_va_end' expects 1 argument, got %zu", got);
        }
        if (!builtin_check_va_list_arg((ASTNode *) vec_get(call_expr->args, 0), ctx))
        {
            return false;
        }
    }
    call_expr->base.expr_type = type_void();
    return true;
}

/* `__builtin_va_arg(ap, type)`: ap must be a va_list (decayed); the type must
   not be void, a record, or an array. The value is an rvalue of that type. */
static bool check_va_arg_expr(ASTVaArgExpr *va, SemanticCtx *ctx)
{
    if (!builtin_check_va_list_arg(va->ap, ctx))
    {
        return false;
    }
    Type *t = type_rvalue(va->type);
    if (t->kind == TYPE_VOID || type_is_record(t) || type_is_array(t))
    {
        return sem_error(ctx, va->base.loc,
                         "'__builtin_va_arg' argument type cannot be void, a record, or an array");
    }
    va->base.expr_type = t;
    return true;
}

/* The interned function type of a signature: the return and each param type
   top-level-unqualified (§6.7.6.3p15), plus the variadic bit. type_func interns
   structurally, so compatible signatures are pointer-equal. */
static Type *build_func_type(FuncSig *sig, SemanticCtx *ctx)
{
    Vec *param_types = vec_new(ctx->arena);
    size_t nparams = vec_size(sig->params);
    for (size_t i = 0; i < nparams; i++)
    {
        ASTVarDecl *param = ast_as(ASTVarDecl, (ASTNode *) vec_get(sig->params, i));
        vec_push(param_types, type_unqual(param->type));
    }
    return type_func(type_unqual(sig->ret_type), param_types, sig->is_variadic);
}

/* Validate a call's argument list and result against a parameter-type list.
   Shared by named calls (params from the AST) and indirect calls (from the
   pointed-to function type). Sets the call's result type. */
static bool check_call_args(ASTCallExpr *call_expr, Type *ret_type, Vec *param_types,
                            bool is_variadic, const char *callee_name, SemanticCtx *ctx)
{
    size_t expected = vec_size(param_types);
    size_t got = vec_size(call_expr->args);
    if (is_variadic)
    {
        /* §6.5.2.2p6: the fixed (named) part is enforced; extra args are
           legal. */
        if (got < expected)
        {
            return sem_error(ctx, call_expr->base.loc,
                             "function '%s' expects at least %zu arguments, got %zu", callee_name,
                             expected, got);
        }
    }
    else if (expected != got)
    {
        return sem_error(ctx, call_expr->base.loc, "function '%s' expects %zu arguments, got %zu",
                         callee_name, expected, got);
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
        if (!type_assignable((Type *) vec_get(param_types, i), arg->expr_type))
        {
            return sem_error(ctx, arg->loc, "incompatible argument type for parameter %zu", i + 1);
        }
    }
    /* The function value is an unqualified rvalue even for a const return
       type (`const int f()`). */
    call_expr->base.expr_type = type_rvalue(ret_type);
    return true;
}

static bool check_indirect_call(ASTCallExpr *call_expr, SemanticCtx *ctx);

static bool check_call_expr(ASTCallExpr *call_expr, SemanticCtx *ctx)
{
    if (call_expr->callee_expr)
    {
        return check_indirect_call(call_expr, ctx);
    }

    ASTNode *callee_node = strmap_get(ctx->globals, call_expr->callee);
    if (callee_node)
    {
        FuncSig *callee = func_sig_of(callee_node);
        /* Reduce the declared AST params to their types (the interned function
           type's identity, exactly as build_func_type does). */
        Vec *param_types = vec_new(ctx->arena);
        size_t nparams = vec_size(callee->params);
        for (size_t i = 0; i < nparams; i++)
        {
            ASTVarDecl *param = ast_as(ASTVarDecl, (ASTNode *) vec_get(callee->params, i));
            vec_push(param_types, type_unqual(param->type));
        }
        return check_call_args(call_expr, callee->ret_type, param_types, callee->is_variadic,
                               call_expr->callee, ctx);
    }

    /* va_start/va_end builtins are never ASTFuncDefs; a user definition of the
       same name wins (the globals lookup above). */
    if (strcmp(call_expr->callee, "__builtin_va_start") == 0 ||
        strcmp(call_expr->callee, "__builtin_va_end") == 0 ||
        strcmp(call_expr->callee, "__builtin_va_copy") == 0)
    {
        return check_va_builtin(call_expr, ctx);
    }

    ASTVarDecl *vdecl = scope_lookup(ctx, call_expr->callee);
    if (!vdecl)
    {
        vdecl = strmap_get(ctx->global_vars, call_expr->callee);
    }
    if (vdecl)
    {
        if (type_is_ptr(vdecl->type) && type_deref(vdecl->type)->kind == TYPE_FUNC)
        {
            /* `fp(x)` where `fp` is a function-pointer variable is an indirect
               call: redirect to the callee-expression form and validate against
               the pointed-to function type. */
            ASTNode *ident = ast_ident(call_expr->callee, call_expr->base.loc, ctx->arena);
            ASTIdent *id = ast_as(ASTIdent, ident);
            id->decl = vdecl;
            id->base.expr_type = type_decay(vdecl->type);
            call_expr->callee_expr = ident;
            call_expr->callee = NULL;
            return check_indirect_call(call_expr, ctx);
        }
        return sem_error(ctx, call_expr->base.loc,
                         "called object '%s' is not a function or function pointer",
                         call_expr->callee);
    }
    return sem_error(ctx, call_expr->base.loc, "undeclared function '%s'", call_expr->callee);
}

static bool check_indirect_call(ASTCallExpr *call_expr, SemanticCtx *ctx)
{
    ASTNode *callee = call_expr->callee_expr;
    if (!check_expr(callee, ctx))
    {
        return false;
    }
    Type *ct = callee->expr_type;
    if (type_is_function(ct))
    {
        ct = type_decay(ct); /* a bare function designator in callee position */
    }
    if (!type_is_ptr(ct) || type_deref(ct)->kind != TYPE_FUNC)
    {
        return sem_error(ctx, callee->loc, "called object is not a function or function pointer");
    }
    Type *fn = type_deref(ct);
    return check_call_args(call_expr, fn->func.ret, fn->func.params, fn->func.is_variadic, "<>",
                           ctx);
}

static bool check_cast_expr(ASTCastExpr *ce, SemanticCtx *ctx)
{
    if (!check_expr(ce->operand, ctx))
    {
        return false;
    }
    if (!sem_resolve_type(&ce->target_type, ctx))
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
        return sem_error(ctx, ce->base.loc, "conversion to non-scalar type requested");
    }
    if (!is_scalar_type(op))
    {
        return sem_error(ctx, ce->base.loc, "invalid cast of non-scalar type");
    }
    /* A cast is never an lvalue; a qualified target equals the unqualified one
       (§6.5.4p4): top-level const drops, pointee qualifiers survive. */
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
            return sem_error(ctx, ma->base.loc, "cannot use '->' on non-pointer type");
        }
        record_type = type_deref(obj_type);
    }
    else
    {
        record_type = obj_type;
    }
    if (!type_is_record(record_type))
    {
        return sem_error(ctx, ma->base.loc, "member access on non-struct/union type");
    }
    if (!type_is_complete(record_type))
    {
        return sem_error(ctx, ma->base.loc, "member access on incomplete type '%s'",
                         record_type->record.tag);
    }
    Type *field_type = type_record_field(record_type, ma->member);
    if (!field_type)
    {
        return sem_error(ctx, ma->base.loc, "no member named '%s'", ma->member);
    }
    u32 bit_offset = 0;
    u32 bit_width = 0;
    ma->is_bitfield = type_record_field_bit(record_type, ma->member, &bit_offset, &bit_width);
    ma->bit_offset = bit_offset;
    ma->bit_width = bit_width;
    /* §6.5.2.3p4: a const-qualified object (or pointer to one) yields const members;
       array members take the qualifier on the element, so `s.a[i]` writes and the
       `const T*` decay work. */
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

static Type *expr_done(bool ok, ASTNode *node)
{
    return ok ? node->expr_type : NULL;
}

static bool check_int_literal(ASTIntLiteral *lit, SemanticCtx *ctx)
{
    ASTNode *node = &lit->base;
    node->expr_type = type_int_literal(lit->value, lit->is_hex, lit->is_unsigned, lit->length);
    (void) ctx;
    return true;
}

/* A floating constant is typed by its suffix (C11 §6.4.4.2). */
static bool check_float_literal(ASTFloatLiteral *fl, SemanticCtx *ctx)
{
    ASTNode *node = &fl->base;
    switch (fl->kind)
    {
        case FK_FLOAT:
            node->expr_type = type_float();
            break;
        case FK_LONG:
            node->expr_type = type_long_double();
            break;
        default:
            node->expr_type = type_double();
            break;
    }
    (void) ctx;
    return true;
}

static bool check_string_literal(ASTStringLiteral *sl, SemanticCtx *ctx)
{
    ASTNode *node = &sl->base;
    node->expr_type = type_decay(type_array(type_char(), sl->length + 1));
    (void) ctx;
    return true;
}

static bool check_subscript_expr(ASTSubscriptExpr *se, SemanticCtx *ctx)
{
    ASTNode *node = &se->base;
    if (!check_expr(se->array, ctx) || !check_expr(se->index, ctx))
    {
        return false;
    }
    Type *ptr_type = type_decay(se->array->expr_type);
    if (!type_is_ptr(ptr_type))
    {
        return sem_error(ctx, node->loc, "subscripted value is not a pointer or array");
    }
    /* §6.5.2.1p1: a subscript index must be an integer, not an FTOI. */
    if (!type_is_integer(type_rvalue(se->index->expr_type)))
    {
        return sem_error(ctx, se->index->loc, "array subscript is not an integer");
    }
    node->expr_type = type_deref(ptr_type);
    return true;
}

/* Resolves a parser-deferred array bound as an integer constant expression
   (§6.6) once expression types are known; VLAs are out of scope. */
static bool sem_resolve_type(Type **slot, SemanticCtx *ctx)
{
    Type *type = *slot;
    if (!type)
    {
        return true;
    }
    if (type_array_is_pending(type))
    {
        ASTNode *bound = type->arr.bound_expr;
        if (!check_expr(bound, ctx))
        {
            return false;
        }
        i64 length;
        if (!fold_integer_constant(bound, &length))
        {
            return sem_error(ctx, bound->loc, "array size must be an integer constant");
        }
        if (length < 0)
        {
            return sem_error(ctx, bound->loc, "array size must not be negative");
        }
        Type *elem = type->arr.elem;
        if (!sem_resolve_type(&elem, ctx))
        {
            return false;
        }
        type->arr.elem = elem;
        *slot = type_array_resolve(type, (u64) length);
        return true;
    }
    if (type->kind == TYPE_ARRAY)
    {
        Type *elem = type->arr.elem;
        if (!sem_resolve_type(&elem, ctx))
        {
            return false;
        }
        if (elem != type->arr.elem)
        {
            *slot = type_array(elem, type->arr.length);
        }
        return true;
    }
    if (type->kind == TYPE_PTR)
    {
        Type *pointee = type->ptr.pointee;
        if (!sem_resolve_type(&pointee, ctx))
        {
            return false;
        }
        if (pointee != type->ptr.pointee)
        {
            *slot = type_ptr(pointee);
        }
        return true;
    }
    return true;
}

static bool check_sizeof_expr(ASTSizeofExpr *se, SemanticCtx *ctx)
{
    ASTNode *node = &se->base;
    if (!check_expr(se->operand, ctx))
    {
        return false;
    }
    Type *op_type = se->operand->expr_type;
    /* §6.3.2.1p3: array-to-pointer decay is suppressed for the direct operand of sizeof. */
    if (se->operand->kind == AST_IDENT)
    {
        ASTIdent *id = ast_as(ASTIdent, se->operand);
        if (id->is_func)
        {
            /* No decay: `sizeof(f)` is sizeof(function type), rejected below. */
            ASTNode *fnode = strmap_get(ctx->globals, id->name);
            op_type = func_sig_of(fnode)->func_type;
        }
        else if (id->decl && type_is_array(id->decl->type))
        {
            op_type = id->decl->type;
        }
    }
    else if (se->operand->kind == AST_STRING_LITERAL)
    {
        /* sizeof a string literal is the array length incl. NUL, not char*. */
        op_type = type_array(type_char(), ast_as(ASTStringLiteral, se->operand)->length + 1);
    }
    else if (se->operand->kind == AST_COMPOUND_LITERAL &&
             type_is_array(ast_as(ASTCompoundLiteral, se->operand)->type))
    {
        /* Compound literals do not decay either (§6.5.2.5p4 note). */
        op_type = ast_as(ASTCompoundLiteral, se->operand)->type;
    }
    else if (se->operand->kind == AST_MEMBER_ACCESS)
    {
        /* §6.5.3.4p1: sizeof suppresses decay, so a member array keeps its extent. */
        Type *ft = ast_as(ASTMemberAccess, se->operand)->field_type;
        if (ft)
        {
            op_type = ft;
        }
    }
    if (!sem_resolve_type(&op_type, ctx))
    {
        return false;
    }
    if (op_type->kind == TYPE_VOID)
    {
        return sem_error(ctx, node->loc, "sizeof(void) is invalid");
    }
    if (type_is_function(op_type))
    {
        return sem_error(ctx, node->loc, "invalid application of 'sizeof' to a function type");
    }
    if (type_is_record(op_type) && !type_is_complete(op_type))
    {
        return sem_error(ctx, node->loc, "sizeof of incomplete type");
    }
    se->size_value = type_sizeof(op_type);
    node->expr_type = type_ulong();
    return true;
}

static bool check_sizeof_type(ASTSizeofType *st, SemanticCtx *ctx)
{
    ASTNode *node = &st->base;
    if (!sem_resolve_type(&st->type, ctx))
    {
        return false;
    }
    if (st->type->kind == TYPE_VOID)
    {
        return sem_error(ctx, node->loc, "sizeof(void) is invalid");
    }
    if (type_is_function(st->type))
    {
        return sem_error(ctx, node->loc, "invalid application of 'sizeof' to a function type");
    }
    if (type_is_record(st->type) && !type_is_complete(st->type))
    {
        return sem_error(ctx, node->loc, "sizeof of incomplete type");
    }
    st->size_value = type_sizeof(st->type);
    node->expr_type = type_ulong();
    return true;
}

static bool check_alignof_expr(ASTAlignofExpr *ae, SemanticCtx *ctx)
{
    ASTNode *node = &ae->base;
    if (!check_expr(ae->operand, ctx))
    {
        return false;
    }
    Type *op_type = ae->operand->expr_type;
    /* §6.3.2.1p3: decay is suppressed for the direct operand of _Alignof, so an
       array aligns as its element type. */
    if (ae->operand->kind == AST_IDENT)
    {
        ASTVarDecl *decl = ast_as(ASTIdent, ae->operand)->decl;
        if (decl && type_is_array(decl->type))
        {
            op_type = decl->type;
        }
    }
    else if (ae->operand->kind == AST_MEMBER_ACCESS)
    {
        Type *ft = ast_as(ASTMemberAccess, ae->operand)->field_type;
        if (ft)
        {
            op_type = ft;
        }
    }
    if (!sem_resolve_type(&op_type, ctx))
    {
        return false;
    }
    if (op_type->kind == TYPE_VOID)
    {
        return sem_error(ctx, node->loc, "_Alignof(void) is invalid");
    }
    if (!type_is_complete(op_type))
    {
        return sem_error(ctx, node->loc, "_Alignof of incomplete type");
    }
    ae->align_value = type_alignof(op_type);
    node->expr_type = type_ulong();
    return true;
}

static bool check_alignof_type(ASTAlignofType *at, SemanticCtx *ctx)
{
    ASTNode *node = &at->base;
    if (!sem_resolve_type(&at->type, ctx))
    {
        return false;
    }
    if (at->type->kind == TYPE_VOID)
    {
        return sem_error(ctx, node->loc, "_Alignof(void) is invalid");
    }
    if (!type_is_complete(at->type))
    {
        return sem_error(ctx, node->loc, "_Alignof of incomplete type");
    }
    at->align_value = type_alignof(at->type);
    node->expr_type = type_ulong();
    return true;
}

static bool check_no_fam_array(Type *type, Loc loc, SemanticCtx *ctx)
{
    if (type_is_array(type) && type_record_has_fam(type_array_elem(type)))
    {
        return sem_error(ctx, loc, "array of a record with a flexible array member");
    }
    return true;
}

static bool check_generic_assoc_type(Type *type, Loc loc, SemanticCtx *ctx)
{
    if (type_is_function(type) || type->kind == TYPE_VOID || !type_is_complete(type))
    {
        return sem_error(ctx, loc, "generic association type must be a complete object type");
    }
    return true;
}

static bool check_generic_selection(ASTGenericSelection *gs, SemanticCtx *ctx)
{
    ASTNode *node = &gs->base;
    if (!check_expr(gs->controlling, ctx))
    {
        return false;
    }
    Type *controlling = type_decay(type_rvalue(gs->controlling->expr_type));

    size_t nassocs = vec_size(gs->assocs);
    for (size_t i = 0; i < nassocs; i++)
    {
        GenericAssoc *assoc = (GenericAssoc *) vec_get(gs->assocs, i);
        if (!check_generic_assoc_type(assoc->type, assoc->expr->loc, ctx))
        {
            return false;
        }
        for (size_t j = 0; j < i; j++)
        {
            GenericAssoc *prev = (GenericAssoc *) vec_get(gs->assocs, j);
            if (type_compatible(prev->type, assoc->type))
            {
                return sem_error(ctx, assoc->expr->loc,
                                 "generic association type is compatible with an earlier one");
            }
        }
    }

    ASTNode *selected = NULL;
    for (size_t i = 0; i < nassocs; i++)
    {
        GenericAssoc *assoc = (GenericAssoc *) vec_get(gs->assocs, i);
        if (type_compatible(controlling, assoc->type))
        {
            selected = assoc->expr;
            break;
        }
    }
    if (!selected)
    {
        selected = gs->default_expr;
    }
    if (!selected)
    {
        return sem_error(ctx, node->loc, "no compatible generic association and no 'default'");
    }
    if (!check_expr(selected, ctx))
    {
        return false;
    }
    gs->selected = selected;
    node->expr_type = selected->expr_type;
    return true;
}

static Type *check_expr(ASTNode *node, SemanticCtx *ctx)
{
    switch (node->kind)
    {
        case AST_INT_LITERAL:
            return expr_done(check_int_literal(ast_as(ASTIntLiteral, node), ctx), node);
        case AST_FLOAT_LITERAL:
            return expr_done(check_float_literal(ast_as(ASTFloatLiteral, node), ctx), node);
        case AST_IDENT:
            return expr_done(check_identifier_expr(ast_as(ASTIdent, node), ctx), node);
        case AST_BINARY_EXPR:
            return expr_done(check_binary_expr(ast_as(ASTBinaryExpr, node), ctx), node);
        case AST_UNARY_EXPR:
            return expr_done(check_unary_expr(ast_as(ASTUnaryExpr, node), ctx), node);
        case AST_INCDEC_EXPR:
            return expr_done(check_incdec_expr(ast_as(ASTIncDecExpr, node), ctx), node);
        case AST_CALL_EXPR:
            return expr_done(check_call_expr(ast_as(ASTCallExpr, node), ctx), node);
        case AST_TERNARY_EXPR:
            return expr_done(check_ternary_expression(ast_as(ASTTernaryExpr, node), ctx), node);
        case AST_SUBSCRIPT_EXPR:
            return expr_done(check_subscript_expr(ast_as(ASTSubscriptExpr, node), ctx), node);
        case AST_SIZEOF_EXPR:
            return expr_done(check_sizeof_expr(ast_as(ASTSizeofExpr, node), ctx), node);
        case AST_SIZEOF_TYPE:
            return expr_done(check_sizeof_type(ast_as(ASTSizeofType, node), ctx), node);
        case AST_ALIGNOF_EXPR:
            return expr_done(check_alignof_expr(ast_as(ASTAlignofExpr, node), ctx), node);
        case AST_ALIGNOF_TYPE:
            return expr_done(check_alignof_type(ast_as(ASTAlignofType, node), ctx), node);
        case AST_STRING_LITERAL:
            return expr_done(check_string_literal(ast_as(ASTStringLiteral, node), ctx), node);
        case AST_MEMBER_ACCESS:
            return expr_done(check_member_access(ast_as(ASTMemberAccess, node), ctx), node);
        case AST_CAST_EXPR:
            return expr_done(check_cast_expr(ast_as(ASTCastExpr, node), ctx), node);
        case AST_VA_ARG_EXPR:
            return expr_done(check_va_arg_expr(ast_as(ASTVaArgExpr, node), ctx), node);
        case AST_COMPOUND_LITERAL:
            return expr_done(check_compound_literal(ast_as(ASTCompoundLiteral, node), ctx), node);
        case AST_GENERIC_SELECTION:
            return expr_done(check_generic_selection(ast_as(ASTGenericSelection, node), ctx), node);
        default:
            sem_error(ctx, node->loc, "unsupported expression kind %s", ast_kind_name(node->kind));
            return NULL;
    }
}

static bool check_return_stmt(ASTReturnStmt *return_stmt, SemanticCtx *ctx, Type *ret_type)
{
    if (ret_type->kind == TYPE_VOID)
    {
        if (return_stmt->expr)
        {
            return sem_error(ctx, return_stmt->base.loc, "void function should not return a value");
        }
    }
    else
    {
        if (!return_stmt->expr)
        {
            return sem_error(ctx, return_stmt->base.loc, "non-void function must return a value");
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
    Type *expr_type = return_stmt->expr ? type_decay(return_stmt->expr->expr_type) : NULL;
    if (return_stmt->expr && type_is_record(ret_type) && !type_assignable(ret_type, expr_type))
    {
        return sem_error(ctx, return_stmt->base.loc,
                         "returning a value incompatible with struct/union return type '%s'",
                         ret_type->record.tag);
    }
    if (return_stmt->expr && type_is_ptr(ret_type) && type_is_ptr(expr_type) &&
        !type_assignable(ret_type, expr_type))
    {
        return sem_error(ctx, return_stmt->base.loc, "incompatible pointer type in return");
    }

    return true;
}

/* A block-scope `extern` names an external-linkage entity (§6.2.2p5); it
   allocates no local storage and resolves through the file-scope namespace,
   not the block locals. */
static bool check_block_extern(ASTVarDecl *var_decl, SemanticCtx *ctx)
{
    if (var_decl->init)
    {
        return sem_error(ctx, var_decl->base.loc, "'%s' has both 'extern' and an initializer",
                         var_decl->name);
    }
    if (strmap_get(ctx->globals, var_decl->name))
    {
        return sem_error(ctx, var_decl->base.loc, "'%s' redeclared as different kind of symbol",
                         var_decl->name);
    }
    ASTVarDecl *existing = strmap_get(ctx->global_vars, var_decl->name);
    if (existing && existing->storage == SC_STATIC)
    {
        return sem_error(ctx, var_decl->base.loc,
                         "extern declaration of '%s' follows static declaration", var_decl->name);
    }
    if (!existing)
    {
        strmap_set(ctx->global_vars, var_decl->name, var_decl);
    }
    return true;
}

/* Check a block-scope automatic variable's initializer: aggregates are planned,
   scalar expressions are checked and type-assigned to the variable. */
static bool check_auto_initializer(ASTVarDecl *var_decl, SemanticCtx *ctx)
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
    if (!check_expr(var_decl->init, ctx) || !check_value_used(var_decl->init, ctx))
    {
        return false;
    }
    Type *init_type = var_decl->init->expr_type;
    if (type_is_record(var_decl->type) && !type_assignable(var_decl->type, init_type))
    {
        return sem_error(ctx, var_decl->base.loc, "invalid initializer for struct/union type '%s'",
                         var_decl->type->record.tag);
    }
    if (type_is_ptr(var_decl->type) && !type_assignable(var_decl->type, init_type))
    {
        return sem_error(ctx, var_decl->base.loc,
                         "incompatible pointer type in initializer for '%s'", var_decl->name);
    }
    return true;
}

static bool check_variable_declaration(ASTVarDecl *var_decl, SemanticCtx *ctx)
{
    if (!sem_resolve_type(&var_decl->type, ctx) ||
        !check_no_fam_array(var_decl->type, var_decl->base.loc, ctx))
    {
        return false;
    }
    if (scope_top_lookup(ctx, var_decl->name))
    {
        return sem_error(ctx, var_decl->base.loc, "redeclaration of '%s'", var_decl->name);
    }
    if (var_decl->type->kind == TYPE_VOID)
    {
        return sem_error(ctx, var_decl->base.loc, "variable '%s' has void type", var_decl->name);
    }
    if (var_decl->storage == SC_STATIC)
    {
        /* Statics are file-backed: the parser folded scalar constants into
           const_init, routed aggregates/strings/addresses into init. */
        if (var_decl->init && plan_var_initializer(ctx, var_decl) == PLAN_ERROR)
        {
            return false;
        }
        if (!check_aggregate_const_init(var_decl, ctx))
        {
            return false;
        }
    }
    if (var_decl->storage == SC_EXTERN)
    {
        return check_block_extern(var_decl, ctx);
    }
    var_decl->is_block_scope = true;
    strmap_set(current_scope(ctx), var_decl->name, var_decl);
    if (var_decl->init && !check_auto_initializer(var_decl, ctx))
    {
        return false;
    }
    /* A `[]` array with no initializer (or an incomplete record/array element)
       stays incomplete — rejected after completion would have run. */
    if (!type_is_complete(var_decl->type))
    {
        return sem_error(ctx, var_decl->base.loc, "variable '%s' has incomplete type",
                         var_decl->name);
    }
    return true;
}

static bool check_expression_statement(ASTExprStmt *expr_stmt, SemanticCtx *ctx)
{
    return check_expr(expr_stmt->expr, ctx);
}

/* Initializer-list planner (C11 §6.7.9):
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
    bool grow; /* root `[]` array: unbounded cursor */
} PlanFrame;

static bool is_aggregate_type(Type *t)
{
    t = type_unqual(t);
    return type_is_array(t) || type_is_record(t);
}

static u32 plan_frame_nchildren(const PlanFrame *fr)
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

static bool plan_frame_child(const PlanFrame *fr, Type **cty, u32 *coff, bool *is_bf, u32 *bit_off,
                             u32 *bit_w)
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
        *is_bf = false;
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
    *is_bf = f->bit_offset >= 0 && f->bit_width >= 0;
    *bit_off = *is_bf ? (u32) f->bit_offset : 0;
    *bit_w = *is_bf ? (u32) f->bit_width : 0;
    return true;
}

static bool record_field_index(Type *agg, const char *name, u32 *out)
{
    agg = type_unqual(agg);
    size_t nf = vec_size(agg->record.fields);
    for (size_t i = 0; i < nf; i++)
    {
        RecordField *f = (RecordField *) vec_get(agg->record.fields, i);
        if (f->name && strcmp(f->name, name) == 0)
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
        if (top->next < plan_frame_nchildren(top))
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
    w->is_bitfield = false;
    w->bit_offset = 0;
    w->bit_width = 0;
    vec_push(plan->writes, w);
    return w;
}

static InitWrite *plan_new_bitfield_write(SemanticCtx *ctx, InitPlan *plan, u32 offset, Type *type,
                                          u32 bit_offset, u32 bit_width, ASTNode *value)
{
    InitWrite *w = arena_alloc(ctx->arena, sizeof(InitWrite), _Alignof(InitWrite));
    w->offset = offset;
    w->type = type;
    w->value = value;
    w->is_string_fill = false;
    w->is_bitfield = true;
    w->bit_offset = bit_offset;
    w->bit_width = bit_width;
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

/* Validate + record a scalar write (initialization bypasses the write gate,
   so const targets are fine). */
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
        return sem_error(ctx, loc, "incompatible type in initializer (target type differs)");
    }
    plan_new_write(ctx, plan, offset, target, value, false);
    return true;
}

static bool plan_bitfield_scalar_write(SemanticCtx *ctx, InitPlan *plan, Type *target, u32 offset,
                                       u32 bit_offset, u32 bit_width, ASTNode *value, Loc loc)
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
        return sem_error(ctx, loc, "incompatible type in initializer (target type differs)");
    }
    plan_new_bitfield_write(ctx, plan, offset, target, bit_offset, bit_width, value);
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
                return sem_error(ctx, loc, "field designator '.%s' used on an array object",
                                 dd->field);
            }
            /* Only the outermost declared-against `[]` array may take an
               out-of-range designator (it sizes the array). Inner brackets stay
               bounded. */
            bool grow = plan->grow_array && cur_ty->arr.length == 0;
            if (dd->index < 0 || (!grow && (u64) dd->index >= cur_ty->arr.length))
            {
                return sem_error(ctx, loc, "array designator index %lld is out of bounds",
                                 (long long) dd->index);
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
            if (dd->kind == ND_INDEX)
            {
                return sem_error(ctx, loc, "array designator used on a non-array object");
            }
            u32 fidx;
            if (!record_field_index(cur_ty, dd->field, &fidx))
            {
                return sem_error(ctx, loc, "no member named '%s' in '%s'", dd->field,
                                 cur_ty->record.tag);
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
            return sem_error(ctx, loc, "cannot apply a designator to a scalar object");
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
            return sem_error(ctx, loc, "array has incomplete type");
        }
        if (type_array_elem(cty)->kind != TYPE_CHAR)
        {
            return sem_error(ctx, loc, "string literal only initializes a char array");
        }
        ASTStringLiteral *sl = ast_as(ASTStringLiteral, value);
        if (sl->length > cty->arr.length)
        {
            return sem_error(ctx, loc, "initializer-string for array of chars is too long");
        }
        plan_new_write(ctx, plan, coff, cty, value, true);
        return true;
    }
    /* `char *` (or another pointer) member/array element: the string decays. */
    return plan_scalar_write(ctx, plan, cty, coff, value, loc);
}

/* §6.7.9p14: a lone string literal fills `char[N]` (`s[5]="hi"` and `{"hi"}` agree);
   an unsized target infers strlen+1, the NUL is kept only if it fits, and
   `strlen > size` is an error (the ir_builder clamps the copy length). */
static PlanResult plan_char_string_clause(SemanticCtx *ctx, InitPlan *plan, Type *t,
                                          ASTInitList *list, InitElem *e, u32 base_off)
{
    if (e->design || e->value->kind != AST_STRING_LITERAL)
    {
        return PLAN_NONE;
    }
    ASTStringLiteral *sl = ast_as(ASTStringLiteral, e->value);
    u64 need = sl->length + 1;
    if (t->arr.length == 0)
    {
        if (!plan->grow_array)
        {
            sem_error(ctx, list->base.loc, "array has incomplete type");
            return PLAN_ERROR;
        }
        plan->inferred_len = need;
        /* The write's type carries only the copy length (strlen+1); recording it
           as the yet-to-be-completed type is safe because plan_brace_list
           resizes the object afterwards. */
        plan_new_write(ctx, plan, base_off, t, e->value, true);
        return PLAN_HANDLED;
    }
    if (sl->length > t->arr.length)
    {
        sem_error(ctx, e->loc, "initializer-string for array of chars is too long");
        return PLAN_ERROR;
    }
    plan_new_write(ctx, plan, base_off, t, e->value, true);
    return PLAN_HANDLED;
}

/* Consume one list element: apply its designator (if any), then plan the clause
   — a nested brace list, a string fill, or a scalar with brace elision (drilling
   to the leaf subobject). `stackp` may be replaced by a designator path. */
static bool plan_elem(SemanticCtx *ctx, InitPlan *plan, Type *t, InitElem *e, u32 base_off,
                      Vec **stackp)
{
    Vec *stack = *stackp;
    if (e->design)
    {
        Vec *path = vec_new(ctx->arena);
        Type *dt;
        u32 doff;
        if (!resolve_designator_path(ctx, plan, t, base_off, e->design, path, &dt, &doff, e->loc))
        {
            return false;
        }
        stack = path;
        *stackp = stack;
    }
    if (vec_size(stack) == 0)
    {
        return sem_error(ctx, e->loc, "excess elements in %s initializer",
                         type_is_array(t) ? "array" : "struct/union");
    }
    PlanFrame *top = (PlanFrame *) vec_last(stack);
    Type *cty;
    u32 coff;
    bool is_bf;
    u32 boff, bw;
    if (!plan_frame_child(top, &cty, &coff, &is_bf, &boff, &bw))
    {
        return sem_error(ctx, e->loc, "excess elements in %s initializer",
                         type_is_array(t) ? "array" : "struct/union");
    }
    if (e->value->kind == AST_INIT_LIST)
    {
        if (!plan_list(ctx, plan, cty, ast_as(ASTInitList, e->value), coff))
        {
            return false;
        }
        cursor_advance(stack);
        return true;
    }
    if (e->value->kind == AST_STRING_LITERAL)
    {
        if (!plan_string_clause(ctx, plan, cty, coff, e->value, e->loc))
        {
            return false;
        }
        cursor_advance(stack);
        return true;
    }
    /* Whole-object copy (§6.7.9p13): a record-typed value initializes the whole
       subobject it designates, matching the `struct T t = s;` and `(struct T){...}`
       forms already handled at the declaration level. The clause must not be
       brace-elided into per-member writes. */
    if (!check_expr(e->value, ctx) || !check_value_used(e->value, ctx))
    {
        return false;
    }
    if (type_is_record(type_rvalue(e->value->expr_type)))
    {
        if (!type_assignable(cty, e->value->expr_type))
        {
            return sem_error(ctx, e->loc, "incompatible type in initializer (target type differs)");
        }
        plan_new_write(ctx, plan, coff, cty, e->value, false);
        cursor_advance(stack);
        return true;
    }
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
        if (!plan_frame_child(top, &cty, &coff, &is_bf, &boff, &bw))
        {
            return sem_error(ctx, e->loc, "excess elements in %s initializer",
                             type_is_array(t) ? "array" : "struct/union");
        }
    }
    if (is_bf)
    {
        if (!plan_bitfield_scalar_write(ctx, plan, cty, coff, boff, bw, e->value, e->loc))
        {
            return false;
        }
    }
    else if (!plan_scalar_write(ctx, plan, cty, coff, e->value, e->loc))
    {
        return false;
    }
    cursor_advance(stack);
    return true;
}

static bool plan_list(SemanticCtx *ctx, InitPlan *plan, Type *t, ASTInitList *list, u32 base_off)
{
    t = type_unqual(t);
    size_t nel = vec_size(list->elems);
    if (nel == 0)
    {
        return true; /* `{}`: zero-init, nothing to write */
    }

    if (type_is_array(t) && type_array_elem(t)->kind == TYPE_CHAR && nel == 1)
    {
        InitElem *e = (InitElem *) vec_get(list->elems, 0);
        PlanResult r = plan_char_string_clause(ctx, plan, t, list, e, base_off);
        if (r == PLAN_ERROR)
        {
            return false;
        }
        if (r == PLAN_HANDLED)
        {
            return true;
        }
    }

    if (is_scalar_type(t))
    {
        if (nel != 1)
        {
            return sem_error(ctx, list->base.loc, "excess elements in scalar initializer");
        }
        InitElem *e = (InitElem *) vec_get(list->elems, 0);
        if (e->design)
        {
            return sem_error(ctx, e->loc, "cannot use a designator with a scalar initializer");
        }
        return plan_scalar_write(ctx, plan, t, base_off, e->value, e->loc);
    }

    if (type_is_array(t) && t->arr.length == 0)
    {
        /* A `[]` rank may only be the outermost, declared-against target:
           it grows from its initializer. Inner empty brackets error. */
        if (!plan->grow_array)
        {
            return sem_error(ctx, list->base.loc, "array has incomplete type");
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
        if (!plan_elem(ctx, plan, t, e, base_off, &stack))
        {
            return false;
        }
        /* The growable root's cursor after the clause is the inferred length: a boundary
           `root.next` counts complete elements; a cursor still inside the current element
           counts `root.next + 1` (a partially-filled row is one element, §6.7.9p22). */
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
        return sem_error(ctx, vd->base.loc, "array '%s' has incomplete type", vd->name);
    }
    if (type_array_elem(arr)->kind != TYPE_CHAR)
    {
        return sem_error(ctx, vd->base.loc, "string-literal initializer requires a 'char' array");
    }
    if (sl->length > type_array_len(arr))
    {
        return sem_error(ctx, vd->base.loc, "initializer-string for array of chars is too long");
    }
    InitPlan *plan = init_plan_new(ctx, arr);
    plan_new_write(ctx, plan, 0, arr, vd->init, true);
    vd->plan = plan;
    return true;
}

/* `int *p = &g;` / `&(type){...}` (block static or file scope): a bare address
   constant in pointer position; the serializer turns it into one 8-byte
   relocation write (§6.6p9 — a compound literal is an address constant). */
static bool plan_ptr_initializer(SemanticCtx *ctx, ASTVarDecl *vd)
{
    if (vd->init->kind != AST_UNARY_EXPR)
    {
        if (vd->init->kind == AST_IDENT)
        {
            /* A bare function designator (`fp = f;`): an address constant whose
               reloc the serializer emits; check_expr resolves the designator. */
            if (!type_is_ptr(vd->type) || type_deref(vd->type)->kind != TYPE_FUNC)
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

/* Run the planner over a brace list, completing a declared-against `[]` array
   through `type_out` first (the interned 0-length type is never mutated).
   Shared by var declarations and compound literals. */
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
   a declared-against `[]` array first. `*handled` is set when `vd->init`
   matched one of these forms. Returns false on error. */
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

/* A declaration initializer that folded to a scalar constant cannot initialize
   an aggregate: its init must be a brace list (§6.7.9p2). */
static bool check_aggregate_const_init(ASTVarDecl *vd, SemanticCtx *ctx)
{
    if (vd->has_const_init && (vd->type->kind == TYPE_ARRAY || type_is_record(vd->type)))
    {
        return sem_error(ctx, vd->base.loc,
                         "aggregate '%s' must be initialized with a brace-enclosed list", vd->name);
    }
    return true;
}

/* Plan a declaration's initializer when it is an aggregate, string, or address
   constant; scalar initializers return PLAN_NONE and are checked as ordinary
   expressions by the caller. */
static PlanResult plan_var_initializer(SemanticCtx *ctx, ASTVarDecl *vd)
{
    bool handled;
    if (!plan_var_aggregate_init(ctx, vd, &handled))
    {
        return PLAN_ERROR;
    }
    if (handled)
    {
        return PLAN_HANDLED;
    }
    if (type_is_fp(type_unqual(vd->type)) && vd->init != NULL)
    {
        /* FP scalars serialize through the shared plan so all FP folds share one path. */
        InitPlan *plan = init_plan_new(ctx, vd->type);
        if (!plan_scalar_write(ctx, plan, type_unqual(vd->type), 0, vd->init, vd->base.loc))
        {
            return PLAN_ERROR;
        }
        vd->plan = plan;
        return PLAN_HANDLED;
    }
    if (vd->init->kind == AST_UNARY_EXPR || vd->init->kind == AST_IDENT)
    {
        return plan_ptr_initializer(ctx, vd) ? PLAN_HANDLED : PLAN_ERROR;
    }
    if (!type_is_ptr(vd->type) || type_deref(vd->type)->kind != TYPE_CHAR)
    {
        sem_error(ctx, vd->base.loc, "string-literal initializer requires a 'char *' variable");
        return PLAN_ERROR;
    }
    return PLAN_NONE;
}

/* A compound literal `(type){ ... }` (§6.5.2.5): an lvalue of the declared type,
   qualifiers intact; initialization itself bypasses the write gate. */
static bool check_compound_literal(ASTCompoundLiteral *cl, SemanticCtx *ctx)
{
    if (!sem_resolve_type(&cl->type, ctx))
    {
        return false;
    }
    Type *ty = type_unqual(cl->type);
    if (ty->kind == TYPE_VOID)
    {
        return sem_error(ctx, cl->base.loc, "conversion to non-scalar type requested");
    }
    if (type_is_record(ty) && !type_is_complete(ty))
    {
        return sem_error(ctx, cl->base.loc, "compound literal of incomplete type");
    }
    if (!plan_brace_list(ctx, &cl->type, cl->init, &cl->plan))
    {
        return false;
    }
    cl->base.expr_type = type_decay(cl->type);
    return true;
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

static bool check_compound_statement(ASTCompoundStmt *compound_stmt, SemanticCtx *ctx,
                                     Type *ret_type)
{
    push_scope(ctx);
    bool ok = check_statement_list(compound_stmt->stmts, ctx, ret_type);
    pop_scope(ctx);
    return ok;
}

static bool check_if_statement(ASTIfStmt *if_stmt, SemanticCtx *ctx, Type *ret_type)
{
    if (!check_condition(ctx, if_stmt->cond))
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
    if (!check_condition(ctx, while_stmt->cond))
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
    if (!check_condition(ctx, do_stmt->cond))
    {
        return false;
    }
    return true;
}

static bool check_for_statement(ASTForStmt *for_stmt, SemanticCtx *ctx, Type *ret_type)
{
    /* C11 §6.8.5p5: a for-init declaration is scoped to the whole loop
       (init, condition, post-expression, body), so `for (int i = ...)` never
       collides with an enclosing or later `i`. */
    push_scope(ctx);
    bool ok = true;
    if (for_stmt->init && !check_stmt(for_stmt->init, ctx, ret_type))
    {
        ok = false;
    }
    else if (for_stmt->cond && !check_condition(ctx, for_stmt->cond))
    {
        ok = false;
    }
    else if (for_stmt->post && !check_expr(for_stmt->post, ctx))
    {
        ok = false;
    }
    if (ok)
    {
        ctx->loop_depth++;
        ok = check_stmt(for_stmt->body, ctx, ret_type);
        ctx->loop_depth--;
    }
    pop_scope(ctx);
    return ok;
}

static bool fold_unary_constant(UnaryOpKind op, i64 v, i64 *out)
{
    switch (op)
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

/* Fold a binary integer-constant operation; division by zero and out-of-range
   shifts are not foldable. `is_unsigned` selects unsigned DIV/REM/SHR and
   relational semantics (§6.3.1.8). */
static bool fold_binary_constant(BinOpKind op, i64 l, i64 r, i64 *out, bool is_unsigned)
{
    switch (op)
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
            if (is_unsigned)
            {
                if (op == BIN_DIV)
                {
                    *out = (i64) ((u64) l / (u64) r);
                }
                else
                {
                    *out = (i64) ((u64) l % (u64) r);
                }
            }
            else
            {
                if (op == BIN_DIV)
                {
                    *out = l / r;
                }
                else
                {
                    *out = l % r;
                }
            }
            return true;
        case BIN_SHL:
            if (r < 0 || r > 63)
            {
                return false;
            }
            *out = (i64) ((u64) l << r);
            return true;
        case BIN_SHR:
            if (r < 0 || r > 63)
            {
                return false;
            }
            *out = is_unsigned ? (i64) ((u64) l >> r) : l >> r;
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
            *out = is_unsigned ? (i64) ((u64) l < (u64) r) : l < r;
            return true;
        case BIN_GT:
            *out = is_unsigned ? (i64) ((u64) l > (u64) r) : l > r;
            return true;
        case BIN_LE:
            *out = is_unsigned ? (i64) ((u64) l <= (u64) r) : l <= r;
            return true;
        case BIN_GE:
            *out = is_unsigned ? (i64) ((u64) l >= (u64) r) : l >= r;
            return true;
        default:
            return false;
    }
}

/* Whether a folded binary operation follows unsigned arithmetic: DIV/REM and
   the relational comparisons use the usual-arithmetic-conversions common type
   of the (promoted) operands; shifts take the promoted left operand. */
static bool fold_binary_unsigned(ASTBinaryExpr *b)
{
    Type *lt = b->left->expr_type ? type_promote(type_rvalue(b->left->expr_type)) : type_int();
    if (b->op == BIN_SHL || b->op == BIN_SHR)
    {
        return type_is_unsigned(lt);
    }
    Type *rt = b->right->expr_type ? type_promote(type_rvalue(b->right->expr_type)) : type_int();
    return type_is_unsigned(type_common(lt, rt));
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
            return fold_integer_constant(u->operand, &v) && fold_unary_constant(u->op, v, out);
        }
        case AST_BINARY_EXPR:
        {
            ASTBinaryExpr *b = ast_as(ASTBinaryExpr, node);
            i64 l, r;
            return fold_integer_constant(b->left, &l) && fold_integer_constant(b->right, &r) &&
                   fold_binary_constant(b->op, l, r, out, fold_binary_unsigned(b));
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
            *out = (i64) ast_as(ASTSizeofType, node)->size_value;
            return true;
        case AST_SIZEOF_EXPR:
            *out = (i64) ast_as(ASTSizeofExpr, node)->size_value;
            return true;
        case AST_ALIGNOF_TYPE:
            *out = (i64) ast_as(ASTAlignofType, node)->align_value;
            return true;
        case AST_ALIGNOF_EXPR:
            *out = (i64) ast_as(ASTAlignofExpr, node)->align_value;
            return true;
        case AST_CAST_EXPR:
        {
            /* Casts are allowed in integer constant expressions (§6.6p6), so
               `case (int)sizeof(x):` folds here; only integer targets fold. */
            ASTCastExpr *ce = ast_as(ASTCastExpr, node);
            i64 v;
            if (!fold_integer_constant(ce->operand, &v) || !type_is_integer(ce->target_type))
            {
                return false;
            }
            *out = type_reduce_int(ce->target_type, v);
            return true;
        }
        case AST_GENERIC_SELECTION:
            return fold_integer_constant(ast_as(ASTGenericSelection, node)->selected, out);
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
        return sem_error(ctx, sw->cond->loc, "switch condition must have integer type");
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
        return sem_error(ctx, cs->base.loc, "'case' label not within a switch statement");
    }
    SwitchSem *sem = (SwitchSem *) vec_last(ctx->switch_sem_stack);
    if (!cs->value_known)
    {
        /* The constant expression couldn't be folded at parse time (e.g.
           `case sizeof(x):`). Resolve types/sizes, then evaluate now. */
        if (!check_expr(cs->expr, ctx) || !fold_integer_constant(cs->expr, &cs->value))
        {
            return sem_error(ctx, cs->base.loc, "case label is not an integer constant expression");
        }
        cs->value_known = true;
    }
    cs->value = type_reduce_int(sem->promoted_cond, cs->value);
    if (u64map_get(sem->values, (u64) cs->value))
    {
        return sem_error(ctx, cs->base.loc, "duplicate case value");
    }
    u64map_set(sem->values, (u64) cs->value, (void *) 1);
    return check_statement_list(cs->stmts, ctx, ret_type);
}

static bool check_default_statement(ASTDefaultStmt *ds, SemanticCtx *ctx, Type *ret_type)
{
    if (ctx->switch_depth == 0)
    {
        return sem_error(ctx, ds->base.loc, "'default' label not within a switch statement");
    }
    SwitchSem *sem = (SwitchSem *) vec_last(ctx->switch_sem_stack);
    if (sem->has_default)
    {
        return sem_error(ctx, ds->base.loc, "multiple default labels in one switch");
    }
    sem->has_default = true;
    return check_statement_list(ds->stmts, ctx, ret_type);
}

static bool check_break_statement(ASTBreakStmt *break_stmt, SemanticCtx *ctx)
{
    (void) break_stmt;
    if (ctx->loop_depth == 0 && ctx->switch_depth == 0)
    {
        return sem_error(ctx, break_stmt->base.loc, "'break' not within a loop or switch");
    }
    return true;
}

static bool check_continue_statement(ASTContinueStmt *continue_stmt, SemanticCtx *ctx)
{
    (void) continue_stmt;
    if (ctx->loop_depth == 0)
    {
        return sem_error(ctx, continue_stmt->base.loc, "'continue' outside of loop");
    }
    return true;
}

static bool check_goto_statement(ASTGotoStmt *goto_stmt, SemanticCtx *ctx)
{
    if (!strmap_get(ctx->labels, goto_stmt->label))
    {
        return sem_error(ctx, goto_stmt->base.loc, "undefined label '%s'", goto_stmt->label);
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
    if (!check_condition(ctx, ternary->cond))
    {
        return false;
    }
    if (!check_expr(ternary->then_expr, ctx) || !check_expr(ternary->else_expr, ctx))
    {
        return false;
    }
    Type *tt = type_decay(type_rvalue(ternary->then_expr->expr_type));
    Type *te = type_decay(type_rvalue(ternary->else_expr->expr_type));
    if (tt->kind == TYPE_VOID && te->kind == TYPE_VOID)
    {
        /* Both branches void: legal discarded-value conditional, e.g. `cond ? f() : (void)0`. */
        ternary->base.expr_type = tt;
        return true;
    }
    if ((tt->kind == TYPE_VOID && !check_value_used(ternary->then_expr, ctx)) ||
        (te->kind == TYPE_VOID && !check_value_used(ternary->else_expr, ctx)))
    {
        return false;
    }
    if (type_is_record(tt) || type_is_record(te))
    {
        /* §6.5.15p5: a conditional on two operands of the same compatible
           structure/union type is valid and selects one operand by value.
           Same-tag records are interned to one Type, so unqualified pointer
           equality is the compatibility check (anonymous records are distinct
           types by §6.7.2.1p7). */
        if (type_is_record(tt) && type_is_record(te) && type_unqual(tt) == type_unqual(te))
        {
            ternary->base.expr_type = type_rvalue(tt);
            return true;
        }
        return sem_error(ctx, ternary->base.loc,
                         "conditional operator on incompatible record types");
    }
    if (type_is_ptr(tt) || type_is_ptr(te))
    {
        /* §6.5.15p6: both operands are pointers to compatible types (or one is a
           null pointer constant). The result picks the pointer side — the
           common case is a function-pointer or data-pointer ternary. */
        ternary->base.expr_type = type_rvalue(type_is_ptr(tt) ? tt : te);
        return true;
    }
    ternary->base.expr_type = type_common(type_promote(tt), type_promote(te));
    return true;
}

/* A typedef names an existing interned type (§6.7.7); the parser registered the
   name, so semantic has nothing to resolve. Function-type targets are legal
   (§6.7.7p3); uses of the name decay or declare a function as usual. */
static bool check_typedef_decl(ASTTypedefDecl *td, SemanticCtx *ctx)
{
    (void) td;
    (void) ctx;
    return true;
}

/* `_Static_assert(expr, "msg")` (§6.7.4): expr must fold to a constant (after
   check_expr resolves its type-dependent subexpressions); the message is
   reported when the value is zero. */
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
        return sem_error(ctx, sa->base.loc,
                         "static assertion expression is not an integer constant expression");
    }
    if (value == 0)
    {
        return sem_error(ctx, sa->base.loc, "static assertion failed: %s", sa->msg);
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
                ASTNode *member = (ASTNode *) vec_get(dl->decls, i);
                if (member->kind == AST_TYPEDEF_DECL)
                {
                    if (!check_typedef_decl(ast_as(ASTTypedefDecl, member), ctx))
                    {
                        return false;
                    }
                    continue;
                }
                if (!check_variable_declaration(ast_as(ASTVarDecl, member), ctx))
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
            return sem_error(ctx, node->loc, "unsupported statement kind %s",
                             ast_kind_name(node->kind));
    }
}

static bool setup_function_params(ASTFuncDef *func_def, SemanticCtx *ctx)
{
    /* Parameters live in the function's outermost scope, which check_func has
       already pushed. */
    size_t nparams = vec_size(func_def->sig.params);
    for (size_t i = 0; i < nparams; i++)
    {
        ASTVarDecl *param = ast_as(ASTVarDecl, (ASTNode *) vec_get(func_def->sig.params, i));
        if (scope_top_lookup(ctx, param->name))
        {
            return sem_error(ctx, param->base.loc, "redeclaration of parameter '%s'", param->name);
        }
        if (param->type->kind == TYPE_VOID)
        {
            return sem_error(ctx, param->base.loc, "parameter '%s' has void type", param->name);
        }
        param->is_block_scope = true;
        strmap_set(current_scope(ctx), param->name, param);
    }
    return true;
}

/* Statement walkers: labels may be referenced before they are defined (goto
   can jump forward), so a function's labels are collected before its body is
   checked. Duplicate labels are reported here, once. */
static void collect_labels(ASTNode *node, SemanticCtx *ctx);

static void collect_labels_statements(Vec *stmts, SemanticCtx *ctx)
{
    size_t n = vec_size(stmts);
    for (size_t i = 0; i < n; i++)
    {
        collect_labels((ASTNode *) vec_get(stmts, i), ctx);
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
            collect_labels_statements(ast_as(ASTCompoundStmt, node)->stmts, ctx);
            break;
        case AST_IF_STMT:
        {
            ASTIfStmt *is = ast_as(ASTIfStmt, node);
            collect_labels(is->then_branch, ctx);
            collect_labels(is->else_branch, ctx);
            break;
        }
        case AST_WHILE_STMT:
            collect_labels(ast_as(ASTWhileStmt, node)->body, ctx);
            break;
        case AST_DO_WHILE_STMT:
            collect_labels(ast_as(ASTDoWhileStmt, node)->body, ctx);
            break;
        case AST_FOR_STMT:
            collect_labels(ast_as(ASTForStmt, node)->body, ctx);
            break;
        case AST_LABEL_STMT:
        {
            ASTLabelStmt *ls = ast_as(ASTLabelStmt, node);
            if (strmap_get(ctx->labels, ls->label))
            {
                sem_error(ctx, ls->base.loc, "redefinition of label '%s'", ls->label);
            }
            else
            {
                strmap_set(ctx->labels, ls->label, ls);
            }
            collect_labels(ls->stmt, ctx);
            break;
        }
        case AST_SWITCH_STMT:
            collect_labels(ast_as(ASTSwitchStmt, node)->body, ctx);
            break;
        case AST_CASE_STMT:
            collect_labels_statements(ast_as(ASTCaseStmt, node)->stmts, ctx);
            break;
        case AST_DEFAULT_STMT:
            collect_labels_statements(ast_as(ASTDefaultStmt, node)->stmts, ctx);
            break;
        default:
            break;
    }
}

/* Check one function definition body, with a fresh label table and loop depth.
   Top-level parameter qualifiers are ignored for function-type compatibility
   (§6.7.6.3p15). */
static bool check_func(ASTNode *node, SemanticCtx *ctx)
{
    ASSERT(node->kind == AST_FUNC_DEF);
    ASTFuncDef *fn = ast_as(ASTFuncDef, node);

    int saved_loop_depth = ctx->loop_depth;
    StrMap *saved_labels = ctx->labels;
    ctx->loop_depth = 0;
    ctx->labels = strmap_new(ctx->arena);

    push_scope(ctx);
    ctx->fn_params = fn->sig.params;
    if (!setup_function_params(fn, ctx))
    {
        pop_scope(ctx);
        ctx->fn_params = NULL;
        ctx->loop_depth = saved_loop_depth;
        ctx->labels = saved_labels;
        return false;
    }
    fn->sig.func_type = build_func_type(&fn->sig, ctx);

    /* Labels may be referenced before they are defined (goto can jump forward),
       so collect them before checking the function body. */
    collect_labels(fn->body, ctx);

    bool result =
        check_statement_list(ast_as(ASTCompoundStmt, fn->body)->stmts, ctx, fn->sig.ret_type);
    if (ctx->error)
    {
        /* An error was reported (e.g. duplicate label during collection);
           fail so the pipeline aborts instead of continuing to IR. */
        result = false;
    }

    pop_scope(ctx);
    ctx->fn_params = NULL;
    ctx->loop_depth = saved_loop_depth;
    ctx->labels = saved_labels;
    return result;
}

/* Validate and register one file-scope variable declaration. Repeated
   tentative/extern declarations merge; the most-defined declaration wins (a
   definition replaces an extern-only one; an initialized definition replaces a
   tentative one); two definitions collide. */
/* Merge a file-scope declaration with a prior one (§6.9.2p2, §6.7.3p8-10):
   linkage and qualifier mismatches, and two constant definitions, are errors. */
static bool merge_global_var(ASTVarDecl *vd, ASTVarDecl *existing, SemanticCtx *ctx)
{
    if ((existing->storage == SC_STATIC) != (vd->storage == SC_STATIC))
    {
        return sem_error(ctx, vd->base.loc, "%s declaration of '%s' follows %s declaration",
                         vd->storage == SC_STATIC ? "static" : "non-static", vd->name,
                         existing->storage == SC_STATIC ? "static" : "non-static");
    }
    if (type_unqual(existing->type) == type_unqual(vd->type) &&
        type_is_const(existing->type) != type_is_const(vd->type))
    {
        return sem_error(ctx, vd->base.loc, "conflicting type qualifiers in declaration of '%s'",
                         vd->name);
    }
    if (existing->has_const_init && vd->has_const_init)
    {
        return sem_error(ctx, vd->base.loc, "redefinition of '%s'", vd->name);
    }
    return true;
}

/* C11 §6.9.2p2: the most-defined declaration wins — a definition replaces an
   extern-only declaration, an initialized definition replaces a tentative one. */
static bool global_replaced(ASTVarDecl *vd, ASTVarDecl *existing)
{
    if (!existing)
    {
        return true;
    }
    if (existing->storage == SC_EXTERN && vd->storage != SC_EXTERN)
    {
        return true;
    }
    return vd->has_const_init && !existing->has_const_init;
}

/* Validate and register one file-scope variable: tentative/extern declarations
   merge, two constant definitions collide, and the most-defined one wins. */
static bool collect_one_global_var(ASTVarDecl *vd, SemanticCtx *ctx)
{
    if (!sem_resolve_type(&vd->type, ctx) || !check_no_fam_array(vd->type, vd->base.loc, ctx))
    {
        return false;
    }
    if (strmap_get(ctx->globals, vd->name))
    {
        return sem_error(ctx, vd->base.loc, "redefinition of '%s' as a global variable", vd->name);
    }
    if (vd->type->kind == TYPE_VOID)
    {
        return sem_error(ctx, vd->base.loc, "variable '%s' has void type", vd->name);
    }
    /* §6.9.2p3: an `extern` declaration may name an incomplete type (the tag
       may complete later in the TU); a definition may not. */
    if (vd->storage != SC_EXTERN && type_is_record(vd->type) && !type_is_complete(vd->type))
    {
        return sem_error(ctx, vd->base.loc, "variable '%s' has incomplete type", vd->name);
    }
    /* §6.2.1p7: the identifier is in scope for its own initializer, so a
       self-referential static (`struct list_head h = { &h, &h }`) resolves. */
    ASTVarDecl *existing = strmap_get(ctx->global_vars, vd->name);
    if (!existing)
    {
        strmap_set(ctx->global_vars, vd->name, vd);
    }
    if (vd->init)
    {
        /* Initializer lists are flattened by the planner (completing a `[]`
           array per §6.7.9); char arrays take a string literal byte-fill;
           bare `char *` pointers keep the .data string-address relocation. */
        if (plan_var_initializer(ctx, vd) == PLAN_ERROR)
        {
            return false;
        }
    }
    if (!check_aggregate_const_init(vd, ctx))
    {
        return false;
    }
    /* A file-scope `[]` array left without an initializer stays incomplete;
       `extern int a[];` declares (not defines) it and is legal. */
    if (vd->storage != SC_EXTERN && type_is_array(vd->type) && !type_is_complete(vd->type))
    {
        return sem_error(ctx, vd->base.loc, "variable '%s' has incomplete type", vd->name);
    }

    /* C11 §6.9.2p2: a declaration with an initializer is a definition even
       with `extern`, so normalize it to a plain external definition. */
    if (vd->storage == SC_EXTERN && (vd->has_const_init || vd->init))
    {
        vd->storage = SC_NONE;
    }

    if (existing && !merge_global_var(vd, existing, ctx))
    {
        return false;
    }
    if (global_replaced(vd, existing))
    {
        strmap_set(ctx->global_vars, vd->name, vd);
    }
    return true;
}

static bool register_globals(ASTProgram *prog, SemanticCtx *ctx)
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
                ASTNode *member = (ASTNode *) vec_get(dl->decls, j);
                if (member->kind != AST_VAR_DECL)
                {
                    continue;
                }
                if (!collect_one_global_var(ast_as(ASTVarDecl, member), ctx))
                {
                    return false;
                }
            }
        }
    }
    return true;
}

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

/* Merge a function declaration/definition with a prior same-named entry
   (§6.7.6.3): linkage and double-definition are errors; compatible signatures
   are pointer-equal interned types; a definition upgrades a prior prototype. */
static bool merge_function_decl(ASTNode *decl, ASTNode *prev, SemanticCtx *ctx)
{
    FuncSig *fn = func_sig_of(decl);
    FuncSig *pfn = func_sig_of(prev);
    if ((pfn->spec.storage == SC_STATIC) != (fn->spec.storage == SC_STATIC))
    {
        return sem_error(ctx, decl->loc, "%s declaration of '%s' follows %s declaration",
                         fn->spec.storage == SC_STATIC ? "static" : "non-static", fn->name,
                         pfn->spec.storage == SC_STATIC ? "static" : "non-static");
    }
    if (func_node_defined(prev) && func_node_defined(decl))
    {
        return sem_error(ctx, decl->loc, "redefinition of '%s'", fn->name);
    }
    if (pfn->func_type != fn->func_type)
    {
        return sem_error(ctx, decl->loc, "conflicting types for '%s'", fn->name);
    }
    if (!func_node_defined(prev) && func_node_defined(decl))
    {
        strmap_set(ctx->globals, fn->name, decl);
    }
    return true;
}

/* Register every function definition and prototype: intern its function type,
   merge/reject against any prior same-named entry. */
static bool register_functions(ASTProgram *prog, SemanticCtx *ctx)
{
    size_t ndecls = vec_size(prog->decls);
    for (size_t i = 0; i < ndecls; i++)
    {
        ASTNode *decl = (ASTNode *) vec_get(prog->decls, i);
        if (decl->kind == AST_DECL_LIST)
        {
            /* A file-scope decl list is all vars or all typedefs; check the latter. */
            ASTDeclList *dl = ast_as(ASTDeclList, decl);
            size_t n = vec_size(dl->decls);
            for (size_t j = 0; j < n; j++)
            {
                ASTNode *member = (ASTNode *) vec_get(dl->decls, j);
                if (member->kind == AST_TYPEDEF_DECL &&
                    !check_typedef_decl(ast_as(ASTTypedefDecl, member), ctx))
                {
                    return false;
                }
            }
            continue;
        }
        if (decl->kind == AST_STRUCT_DECL || decl->kind == AST_ENUM_DECL ||
            decl->kind == AST_VAR_DECL || decl->kind == AST_STATIC_ASSERT)
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
        if (decl->kind != AST_FUNC_DEF && decl->kind != AST_FUNC_DECL)
        {
            return sem_error(ctx, decl->loc,
                             "expected function definition or prototype at top level");
        }
        FuncSig *fn = func_sig_of(decl);
        fn->func_type = build_func_type(fn, ctx);
        ASTNode *prev = strmap_get(ctx->globals, fn->name);
        if (prev)
        {
            if (!merge_function_decl(decl, prev, ctx))
            {
                return false;
            }
        }
        else
        {
            if (strmap_get(ctx->global_vars, fn->name))
            {
                return sem_error(ctx, decl->loc, "redefinition of '%s'", fn->name);
            }
            strmap_set(ctx->globals, fn->name, decl);
        }
    }
    return true;
}

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

ASTNode *semantic_check(ASTNode *ast, const SemanticConfig *cfg, Arena *arena)
{
    if (!ast)
    {
        return NULL;
    }

    SemanticConfig sc = (SemanticConfig) {0};
    if (cfg)
    {
        sc = *cfg;
    }

    SemanticCtx ctx = {
        .arena = arena,
        .globals = strmap_new(arena),
        .global_vars = strmap_new(arena),
        .scopes = vec_new(arena),
        .labels = NULL,
        .loop_depth = 0,
        .switch_depth = 0,
        .switch_sem_stack = vec_new(arena),
        .cfg = sc,
        .error = false,
    };

    if (ast->kind != AST_PROGRAM)
    {
        sem_error(&ctx, ast->loc, "expected program at top level");
        return NULL;
    }

    ASTProgram *prog = ast_as(ASTProgram, ast);

    if (!check_file_scope_asserts(prog, &ctx))
    {
        return NULL;
    }

    /* Functions are registered before globals so file-scope initializers may
       reference function designators; cross-map collisions are caught by each
       pass's reverse-map check. */
    if (!register_functions(prog, &ctx))
    {
        return NULL;
    }

    if (!register_globals(prog, &ctx))
    {
        return NULL;
    }

    if (!check_function_bodies(prog, &ctx))
    {
        return NULL;
    }

    return ast;
}
