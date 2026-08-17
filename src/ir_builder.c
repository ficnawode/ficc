#include "ir_builder.h"
#include "util/hashmap.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct PendingPhi PendingPhi;
struct PendingPhi
{
    const char *name;
    u32 vreg;
};

typedef struct BlockLocals BlockLocals;
struct BlockLocals
{
    StrMap *locals;    /* name -> IrOperand* */
    Vec *pending_phis; /* Vec<PendingPhi*> */
};

typedef struct ExprResult ExprResult;
struct ExprResult
{
    IrOperand value;
    IrBlock *block;
};

typedef struct LoopBlocks LoopBlocks;
struct LoopBlocks
{
    IrBlock *header;
    IrBlock *body;
    IrBlock *latch;
    IrBlock *exit;
};

typedef struct LoopContext LoopContext;
struct LoopContext
{
    IrBlock *continue_target;
    IrBlock *exit;
};

typedef struct FuncBuilder FuncBuilder;
struct FuncBuilder
{
    IrModule *mod;
    U64Map *block_locals; /* (u64)IrBlock* -> BlockLocals* */
    Vec *loop_stack;      /* Vec<LoopContext*> */
    StrMap *goto_labels;  /* label name -> IrBlock* */
    StrMap *var_types;    /* variable name -> Type* */
    StrMap *func_types;   /* function name -> Type* (return type) */
};

static IrOperand resolve_variable(FuncBuilder *ctx, const char *name, IrBlock *bb);
static IrOperand build_phi(FuncBuilder *ctx, const char *name, IrBlock *bb);
static IrBlock *label_block(FuncBuilder *ctx, IrFunction *f, const char *name);
static ExprResult build_expr(ASTNode *node, IrFunction *f, IrBlock *bb, FuncBuilder *ctx);
static ExprResult build_deref_expr(ASTUnaryExpr *ue, IrFunction *f, IrBlock *bb, FuncBuilder *ctx);
static ExprResult build_addr_expr(ASTUnaryExpr *ue, IrFunction *f, IrBlock *bb, FuncBuilder *ctx);
static ExprResult build_subscript_expr(ASTSubscriptExpr *se, IrFunction *f, IrBlock *bb,
                                       FuncBuilder *ctx);
static ExprResult build_string_literal_expr(ASTStringLiteral *sl, IrFunction *f, IrBlock *bb,
                                            FuncBuilder *ctx);
static IrBlock *build_stmt(ASTNode *node, IrFunction *f, IrBlock *bb, FuncBuilder *ctx);

static void ir_error(ASTNode *node, const char *fmt, ...)
{
    Loc loc = node->loc;
    fprintf(stderr, "%s:%u:%u: [ir] error: ", loc.file, loc.line, loc.col);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

static IrOperand *box_operand(FuncBuilder *ctx, IrOperand op)
{
    IrOperand *p = arena_alloc(ctx->mod->arena, sizeof(IrOperand), sizeof(IrOperand));
    *p = op;
    return p;
}

static u32 alloc_vreg_from_type(FuncBuilder *ctx, Type *type)
{
    u8 width = type->width / 8;
    ASSERT(width == 1 || width == 2 || width == 4 || width == 8);
    return ir_alloc_vreg(ctx->mod, width);
}

static inline Type *node_type(ASTNode *n)
{
    return n->expr_type ? n->expr_type : type_int();
}

static IrOperand promote_to(FuncBuilder *ctx, IrBlock *bb, IrOperand val, Type *src_type,
                            Type *target_type)
{
    u8 src_w = src_type->width / 8;
    u8 tgt_w = target_type->width / 8;
    if (val.is_imm)
    {
        u32 src_vreg = alloc_vreg_from_type(ctx, src_type);
        ir_emit_unary(bb, type_is_signed(src_type) ? OP_SEXT : OP_ZEXT, src_vreg, val);
        val = ir_operand_vreg(src_vreg);
    }
    if (src_type->kind == target_type->kind || src_w == tgt_w)
    {
        return val;
    }
    if (tgt_w > src_w)
    {
        u32 dst = alloc_vreg_from_type(ctx, target_type);
        IrOpcode op = type_is_signed(src_type) ? OP_SEXT : OP_ZEXT;
        ir_emit_unary(bb, op, dst, val);
        return ir_operand_vreg(dst);
    }
    else if (tgt_w < src_w)
    {
        u32 dst = alloc_vreg_from_type(ctx, target_type);
        ir_emit_unary(bb, OP_TRUNC, dst, val);
        return ir_operand_vreg(dst);
    }
    return val;
}

static bool is_comparison_op(BinOpKind op)
{
    return op >= BIN_EQ && op <= BIN_GE;
}

static bool is_shift_op(BinOpKind op)
{
    return op == BIN_SHL || op == BIN_SHR;
}

static bool is_divrem_op(BinOpKind op)
{
    return op == BIN_DIV || op == BIN_REM;
}

static u32 alloc_phi_vreg(FuncBuilder *ctx, const char *name)
{
    Type *t = strmap_get(ctx->var_types, name);
    if (!t)
    {
        ir_error(NULL, "PHI for undeclared variable '%s' (internal IR error)", name);
        ASSERT(false);
        return alloc_vreg_from_type(ctx, type_int());
    }
    /* Arrays are stored as a decayed pointer in SSA; allocate the phi slot
       with the pointer width, not the element width. */
    return alloc_vreg_from_type(ctx, type_decay(t));
}

static BlockLocals *get_block_locals(FuncBuilder *ctx, IrBlock *bb)
{
    BlockLocals *bl = u64map_get(ctx->block_locals, (u64) (uintptr_t) bb);
    if (!bl)
    {
        bl = arena_alloc(ctx->mod->arena, sizeof(BlockLocals), sizeof(void *));
        bl->locals = strmap_new(ctx->mod->arena);
        bl->pending_phis = vec_new(ctx->mod->arena);
        u64map_set(ctx->block_locals, (u64) (uintptr_t) bb, bl);
    }
    return bl;
}

static IrBlock *new_block(IrFunction *f, const char *prefix)
{
    size_t idx = vec_size(f->blocks);
    size_t len = strlen(prefix);
    char *buf = arena_alloc(f->arena, len + 32, sizeof(char));
    snprintf(buf, len + 32, "%s_%zu", prefix, idx);
    return ir_func_add_block(f, buf);
}

static bool is_terminated(IrBlock *bb)
{
    size_t n = vec_size(bb->instrs);
    if (n == 0)
    {
        return false;
    }
    IrInstr *last = (IrInstr *) vec_get(bb->instrs, n - 1);
    return last->opcode == OP_RET || last->opcode == OP_UNREACHABLE || last->opcode == OP_BR ||
           last->opcode == OP_BRCOND;
}

static void jump(IrBlock *from, IrBlock *to)
{
    ir_emit_br(from, to->label);
    vec_push(to->preds, from);
}

static void cond_jump(IrBlock *from, IrOperand cond, IrBlock *then_bb, IrBlock *else_bb)
{
    ir_emit_brcond(from, cond, then_bb->label, else_bb->label);
    vec_push(then_bb->preds, from);
    vec_push(else_bb->preds, from);
}

static void declare_pred(IrBlock *bb, IrBlock *pred)
{
    vec_push(bb->preds, pred);
}

static IrOperand read_variable(FuncBuilder *ctx, const char *name, IrBlock *bb)
{
    BlockLocals *bl = get_block_locals(ctx, bb);
    IrOperand *p = strmap_get(bl->locals, name);
    if (p)
    {
        return *p;
    }
    return resolve_variable(ctx, name, bb);
}

static IrOperand resolve_variable(FuncBuilder *ctx, const char *name, IrBlock *bb)
{
    BlockLocals *bl = get_block_locals(ctx, bb);
    size_t npreds = vec_size(bb->preds);
    IrOperand val;
    if (npreds == 0)
    {
        /* No predecessors: undefined (semantic already checked) */
        val = ir_operand_imm(0);
    }
    else if (npreds == 1 && !bb->is_loop_header)
    {
        /* Single predecessor and not a loop header: no PHI needed. Read the value
           from the predecessor. For an unsealed predecessor this creates a
           placeholder that will be filled when that block is sealed. */
        IrBlock *pred = (IrBlock *) vec_get(bb->preds, 0);
        val = read_variable(ctx, name, pred);
    }
    else if (bb->sealed && !bb->is_loop_header)
    {
        /* Sealed non-loop block with multiple predecessors: build a PHI now. */
        val = build_phi(ctx, name, bb);
    }
    else
    {
        /* Unsealed block with multiple predecessors, or a loop header: create a
           placeholder PHI that will be filled when the block is sealed. Loop
           headers are pre-emptively treated as merge points because their back
           edge is added after the body is built. */
        u32 dst = alloc_phi_vreg(ctx, name);
        PendingPhi *ip = arena_alloc(ctx->mod->arena, sizeof(PendingPhi), sizeof(void *));
        ip->name = name;
        ip->vreg = dst;
        vec_push(bl->pending_phis, ip);
        val = ir_operand_vreg(dst);
    }
    strmap_set(bl->locals, name, box_operand(ctx, val));
    return val;
}

static void write_variable(FuncBuilder *ctx, const char *name, IrBlock *bb, IrOperand val)
{
    BlockLocals *bl = get_block_locals(ctx, bb);
    strmap_set(bl->locals, name, box_operand(ctx, val));
}

static IrInstr *emit_phi_at_start(IrBlock *bb, u32 dst, u32 nentries)
{
    IrInstr *phi = ir_emit_phi(bb, dst, nentries);
    IrInstr *last = (IrInstr *) vec_pop(bb->instrs);
    ASSERT(last == phi);
    vec_insert(bb->instrs, 0, phi);
    return phi;
}

static void fill_phi_entries(FuncBuilder *ctx, IrBlock *bb, const char *name, IrInstr *phi)
{
    size_t n = vec_size(bb->preds);
    for (size_t e = 0; e < n; e++)
    {
        IrBlock *pred = (IrBlock *) vec_get(bb->preds, e);
        IrOperand pval = read_variable(ctx, name, pred);
        ir_phi_add_entry(phi, pval, pred);
    }
}

static IrOperand build_phi(FuncBuilder *ctx, const char *name, IrBlock *bb)
{
    u32 nentries = (u32) vec_size(bb->preds);
    u32 dst = alloc_phi_vreg(ctx, name);
    IrInstr *phi = emit_phi_at_start(bb, dst, nentries);
    fill_phi_entries(ctx, bb, name, phi);
    return ir_operand_vreg(dst);
}

static void insert_phi(FuncBuilder *ctx, IrBlock *bb, const char *name, u32 dst)
{
    u32 nentries = (u32) vec_size(bb->preds);
    IrInstr *phi = emit_phi_at_start(bb, dst, nentries);
    fill_phi_entries(ctx, bb, name, phi);
}

static void seal_block(FuncBuilder *ctx, IrBlock *bb)
{
    if (bb->sealed)
    {
        return;
    }
    bb->sealed = true;

    BlockLocals *bl = get_block_locals(ctx, bb);
    Vec *phis = bl->pending_phis;
    size_t n = vec_size(phis);
    for (size_t i = 0; i < n; i++)
    {
        PendingPhi *ip = (PendingPhi *) vec_get(phis, i);
        insert_phi(ctx, bb, ip->name, ip->vreg);
    }
}

static const IrOpcode binop_ir[] = {
    [BIN_ADD] = OP_ADD, [BIN_SUB] = OP_SUB, [BIN_MUL] = OP_MUL, [BIN_AND] = OP_AND,
    [BIN_OR] = OP_OR,   [BIN_XOR] = OP_XOR, [BIN_SHL] = OP_SHL, [BIN_SHR] = OP_ASHR,
    /* BIN_EQ..BIN_GE, BIN_DIV, BIN_REM resolved in build_arith_binop_expr */
};

static const IrOpcode unary_opcodes[] = {
    [UN_NEG] = OP_NEG,
    [UN_BIT_NOT] = OP_NOT,
};

static void emit_unary_op(IrBlock *bb, u32 dst, UnaryOpKind op, IrOperand src, ASTNode *node)
{
    IrOpcode code = unary_opcodes[op];
    if (code == 0)
    {
        ir_error(node, "unsupported unary operator");
        return;
    }
    ir_emit_unary(bb, code, dst, src);
}

static ExprResult expr_result(IrOperand value, IrBlock *bb)
{
    ExprResult r = {value, bb};
    return r;
}

static ExprResult build_int_literal_expr(ASTIntLiteral *lit, IrFunction *f, IrBlock *bb,
                                         FuncBuilder *ctx)
{
    (void) f;
    (void) ctx;
    return expr_result(ir_operand_imm(lit->value), bb);
}

static ExprResult build_ident_expr(ASTIdent *id, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    (void) f;
    return expr_result(read_variable(ctx, id->name, bb), bb);
}

static ExprResult build_short_circuit(ASTBinaryExpr *be, IrFunction *f, IrBlock *bb,
                                      FuncBuilder *ctx)
{
    bool is_or = be->op == BIN_LOG_OR;
    const char *pfx = is_or ? "lor" : "land";
    ExprResult left = build_expr(be->left, f, bb, ctx);

    char name[16];
    IrBlock *true_bb = new_block(f, is_or ? "lor_true" : "land_true");
    IrBlock *false_bb = new_block(f, is_or ? "lor_false" : "land_false");
    snprintf(name, sizeof(name), "%s_rhs", pfx);
    IrBlock *right_bb = new_block(f, name);
    snprintf(name, sizeof(name), "%s_merge", pfx);
    IrBlock *merge_bb = new_block(f, name);

    if (is_or)
    {
        cond_jump(left.block, left.value, true_bb, right_bb);
    }
    else
    {
        cond_jump(left.block, left.value, right_bb, false_bb);
    }

    ExprResult right = build_expr(be->right, f, right_bb, ctx);
    cond_jump(right.block, right.value, true_bb, false_bb);

    jump(true_bb, merge_bb);
    jump(false_bb, merge_bb);

    /* Seal the short-circuit blocks before the merge so that values flowing
       into the merge come from sealed predecessors. */
    seal_block(ctx, left.block);
    seal_block(ctx, right_bb);
    seal_block(ctx, false_bb);
    seal_block(ctx, true_bb);

    /* The merge block is sealed so future reads produce PHIs. */
    merge_bb->sealed = true;

    u32 dst = alloc_vreg_from_type(ctx, type_int());
    IrInstr *phi = emit_phi_at_start(merge_bb, dst, 2);
    ir_phi_add_entry(phi, ir_operand_imm(1), true_bb);
    ir_phi_add_entry(phi, ir_operand_imm(0), false_bb);

    return expr_result(ir_operand_vreg(dst), merge_bb);
}

static ExprResult build_ternary_expr(ASTTernaryExpr *te, IrFunction *f, IrBlock *bb,
                                     FuncBuilder *ctx)
{
    ExprResult cond = build_expr(te->cond, f, bb, ctx);

    IrBlock *then_bb = new_block(f, "tern_then");
    IrBlock *else_bb = new_block(f, "tern_else");
    IrBlock *merge_bb = new_block(f, "tern_merge");

    cond_jump(cond.block, cond.value, then_bb, else_bb);

    ExprResult then_val = build_expr(te->then_expr, f, then_bb, ctx);
    if (!is_terminated(then_val.block))
    {
        jump(then_val.block, merge_bb);
    }

    ExprResult else_val = build_expr(te->else_expr, f, else_bb, ctx);
    if (!is_terminated(else_val.block))
    {
        jump(else_val.block, merge_bb);
    }

    Type *tern_type = node_type((ASTNode *) te);
    u32 dst = alloc_vreg_from_type(ctx, tern_type);
    IrInstr *phi = emit_phi_at_start(merge_bb, dst, 2);
    ir_phi_add_entry(phi, then_val.value, then_val.block);
    ir_phi_add_entry(phi, else_val.value, else_val.block);
    merge_bb->sealed = true;

    return expr_result(ir_operand_vreg(dst), merge_bb);
}

static ExprResult build_assign_expr(ASTBinaryExpr *be, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    ASTNode *target = be->left;

    if (target->kind == AST_IDENT)
    {
        ExprResult left = build_expr(target, f, bb, ctx);
        bb = left.block;
        ExprResult right = build_expr(be->right, f, bb, ctx);
        bb = right.block;
        ASTIdent *id = ast_as(ASTIdent, target);
        Type *lhs_type = strmap_get(ctx->var_types, id->name);
        ASSERT(lhs_type != NULL);
        Type *rhs_type = node_type(be->right);
        IrOperand val = promote_to(ctx, bb, right.value, rhs_type, lhs_type);
        write_variable(ctx, id->name, bb, val);
        return expr_result(val, bb);
    }

    if (target->kind == AST_UNARY_EXPR && ast_as(ASTUnaryExpr, target)->op == UN_DEREF)
    {
        ASTUnaryExpr *ue = ast_as(ASTUnaryExpr, target);
        ExprResult ptr_res = build_expr(ue->operand, f, bb, ctx);
        bb = ptr_res.block;
        ExprResult right = build_expr(be->right, f, bb, ctx);
        bb = right.block;
        Type *result_type = node_type(target);
        Type *rhs_type = node_type(be->right);
        IrOperand val = promote_to(ctx, bb, right.value, rhs_type, result_type);
        ir_emit_store(bb, val, ptr_res.value, result_type->size);
        return expr_result(val, bb);
    }

    if (target->kind == AST_SUBSCRIPT_EXPR)
    {
        ASTSubscriptExpr *se = ast_as(ASTSubscriptExpr, target);
        ExprResult base = build_expr(se->array, f, bb, ctx);
        ExprResult index = build_expr(se->index, f, base.block, ctx);
        bb = index.block;
        ExprResult right = build_expr(be->right, f, bb, ctx);
        bb = right.block;
        Type *ptr_type = type_decay(node_type(se->array));
        Type *elem = type_deref(ptr_type);
        IrOperand idx = promote_to(ctx, bb, index.value, node_type(se->index), type_long());
        u32 addr = alloc_vreg_from_type(ctx, type_ptr(elem));
        ir_emit_gep(bb, addr, base.value, idx, elem->size);
        Type *rhs_type = node_type(be->right);
        IrOperand val = promote_to(ctx, bb, right.value, rhs_type, elem);
        ir_emit_store(bb, val, ir_operand_vreg(addr), elem->size);
        return expr_result(val, bb);
    }

    ir_error(&be->base, "assignment target must be a variable, dereference, or subscript");
    return expr_result(ir_operand_imm(0), bb);
}

static ExprResult build_arith_binop_expr(ASTBinaryExpr *be, IrFunction *f, IrBlock *bb,
                                         FuncBuilder *ctx)
{
    ExprResult left = build_expr(be->left, f, bb, ctx);
    ExprResult right = build_expr(be->right, f, left.block, ctx);

    Type *lt = node_type(be->left);
    Type *rt = node_type(be->right);

    if (type_is_ptr(lt) && (be->op == BIN_ADD || be->op == BIN_SUB) && !type_is_ptr(rt))
    {
        IrOperand lhs = left.value;
        IrOperand rhs = right.value;
        Type *elem = type_deref(lt);
        rhs = promote_to(ctx, right.block, rhs, rt, type_long());
        if (be->op == BIN_SUB)
        {
            u32 neg_vreg = alloc_vreg_from_type(ctx, type_long());
            ir_emit_unary(right.block, OP_NEG, neg_vreg, rhs);
            rhs = ir_operand_vreg(neg_vreg);
        }
        u32 gep_vreg = alloc_vreg_from_type(ctx, lt);
        ir_emit_gep(right.block, gep_vreg, lhs, rhs, elem->size);
        return expr_result(ir_operand_vreg(gep_vreg), right.block);
    }

    IrOperand lhs = left.value;
    IrOperand rhs = right.value;

    if (!is_comparison_op(be->op) && !is_shift_op(be->op) && !is_divrem_op(be->op))
    {
        Type *promoted = type_common(type_promote(lt), type_promote(rt));
        lhs = promote_to(ctx, right.block, lhs, lt, promoted);
        lt = promoted;
        rhs = promote_to(ctx, right.block, rhs, rt, promoted);
        rt = promoted;
    }
    else if (is_comparison_op(be->op) || is_divrem_op(be->op))
    {
        Type *promoted = type_common(type_promote(lt), type_promote(rt));
        lhs = promote_to(ctx, right.block, lhs, lt, promoted);
        lt = promoted;
        rhs = promote_to(ctx, right.block, rhs, rt, promoted);
        rt = promoted;
    }
    /* For shifts, the left operand type determines the result width;
       the shift count only undergoes integer promotion (C11 §6.5.7). */
    else
    {
        rhs = promote_to(ctx, right.block, rhs, rt, type_promote(rt));
        rt = type_promote(rt);
    }

    IrOpcode op = binop_ir[be->op];

    if (is_comparison_op(be->op))
    {
        bool unsig = type_is_unsigned(lt);
        switch (be->op)
        {
            case BIN_EQ:
                op = OP_ICMP_EQ;
                break;
            case BIN_NE:
                op = OP_ICMP_NE;
                break;
            case BIN_LT:
                op = unsig ? OP_ICMP_ULT : OP_ICMP_SLT;
                break;
            case BIN_GT:
                op = unsig ? OP_ICMP_UGT : OP_ICMP_SGT;
                break;
            case BIN_LE:
                op = unsig ? OP_ICMP_ULE : OP_ICMP_SLE;
                break;
            case BIN_GE:
                op = unsig ? OP_ICMP_UGE : OP_ICMP_SGE;
                break;
            default:
                break;
        }
    }
    else if (is_divrem_op(be->op))
    {
        if (type_is_unsigned(lt))
        {
            op = (be->op == BIN_DIV) ? OP_UDIV : OP_UREM;
        }
        else
        {
            op = (be->op == BIN_DIV) ? OP_SDIV : OP_SREM;
        }
    }
    else if (be->op == BIN_SHR)
    {
        op = type_is_unsigned(lt) ? OP_LSHR : OP_ASHR;
    }

    if (op == 0)
    {
        ir_error(&be->base, "unsupported binary operator");
        return expr_result(ir_operand_imm(0), right.block);
    }

    Type *result_type = node_type((ASTNode *) be);
    u32 dst = alloc_vreg_from_type(ctx, result_type);
    ir_emit_binop(right.block, op, dst, lhs, rhs);
    return expr_result(ir_operand_vreg(dst), right.block);
}

static ExprResult build_unary_expr(ASTUnaryExpr *ue, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    if (ue->op == UN_DEREF)
    {
        return build_deref_expr(ue, f, bb, ctx);
    }
    if (ue->op == UN_ADDR)
    {
        return build_addr_expr(ue, f, bb, ctx);
    }
    ExprResult src = build_expr(ue->operand, f, bb, ctx);
    Type *result_type = node_type((ASTNode *) ue);
    u32 dst = alloc_vreg_from_type(ctx, result_type);
    if (ue->op == UN_LOG_NOT)
    {
        ir_emit_binop(src.block, OP_ICMP_EQ, dst, src.value, ir_operand_imm(0));
    }
    else
    {
        Type *op_type = node_type(ue->operand);
        Type *promoted = type_promote(op_type);
        IrOperand promoted_op = promote_to(ctx, src.block, src.value, op_type, promoted);
        emit_unary_op(src.block, dst, ue->op, promoted_op, &ue->base);
    }
    return expr_result(ir_operand_vreg(dst), src.block);
}

static ExprResult build_call_expr(ASTCallExpr *ce, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    u32 nargs = (u32) vec_size(ce->args);
    IrOperand *args = arena_alloc(ctx->mod->arena, nargs * sizeof(IrOperand), sizeof(IrOperand));
    Type *callee_ret = strmap_get(ctx->func_types, ce->callee);
    ASSERT(callee_ret != NULL);
    IrFunction *callee_ir = NULL;
    size_t nfuncs = vec_size(ctx->mod->funcs);
    for (size_t fi = 0; fi < nfuncs; fi++)
    {
        IrFunction *cf = (IrFunction *) vec_get(ctx->mod->funcs, fi);
        if (strcmp(cf->name, ce->callee) == 0)
        {
            callee_ir = cf;
            break;
        }
    }
    for (u32 i = 0; i < nargs; i++)
    {
        ASTNode *arg = (ASTNode *) vec_get(ce->args, i);
        ExprResult arg_res = build_expr(arg, f, bb, ctx);
        bb = arg_res.block;
        Type *arg_type = arg->expr_type;
        Type *param_type = NULL;
        if (callee_ir && i < vec_size(callee_ir->params))
        {
            IrParam *p = (IrParam *) vec_get(callee_ir->params, i);
            param_type = p->type;
        }
        else
        {
            param_type = arg_type;
        }
        args[i] = promote_to(ctx, bb, arg_res.value, arg_type, param_type);
    }
    u32 dst = callee_ret->kind == TYPE_VOID ? NO_VREG : alloc_vreg_from_type(ctx, callee_ret);
    ir_emit_call(bb, dst, ce->callee, nargs, args);
    return expr_result(dst == NO_VREG ? ir_operand_imm(0) : ir_operand_vreg(dst), bb);
}

static ExprResult build_deref_expr(ASTUnaryExpr *ue, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    ExprResult ptr_res = build_expr(ue->operand, f, bb, ctx);
    Type *result_type = node_type((ASTNode *) ue);
    u32 dst = alloc_vreg_from_type(ctx, result_type);
    ir_emit_load(ptr_res.block, dst, ptr_res.value);
    return expr_result(ir_operand_vreg(dst), ptr_res.block);
}

static ExprResult build_addr_expr(ASTUnaryExpr *ue, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    ASTNode *operand = ue->operand;
    if (operand->kind == AST_UNARY_EXPR && ast_as(ASTUnaryExpr, operand)->op == UN_DEREF)
    {
        return build_expr(ast_as(ASTUnaryExpr, operand)->operand, f, bb, ctx);
    }
    if (operand->kind == AST_SUBSCRIPT_EXPR)
    {
        ASTSubscriptExpr *se = ast_as(ASTSubscriptExpr, operand);
        ExprResult base = build_expr(se->array, f, bb, ctx);
        ExprResult index = build_expr(se->index, f, base.block, ctx);
        bb = index.block;
        Type *ptr_type = type_decay(node_type(se->array));
        Type *elem = type_deref(ptr_type);
        IrOperand idx = promote_to(ctx, bb, index.value, node_type(se->index), type_long());
        u32 addr = alloc_vreg_from_type(ctx, type_ptr(elem));
        ir_emit_gep(bb, addr, base.value, idx, elem->size);
        return expr_result(ir_operand_vreg(addr), bb);
    }
    /* &x for array: build_expr already decays to pointer */
    return build_expr(operand, f, bb, ctx);
}

static ExprResult build_subscript_expr(ASTSubscriptExpr *se, IrFunction *f, IrBlock *bb,
                                       FuncBuilder *ctx)
{
    ExprResult base = build_expr(se->array, f, bb, ctx);
    ExprResult index = build_expr(se->index, f, base.block, ctx);
    bb = index.block;
    Type *ptr_type = type_decay(node_type(se->array));
    Type *elem = type_deref(ptr_type);
    IrOperand idx = promote_to(ctx, bb, index.value, node_type(se->index), type_long());
    u32 addr = alloc_vreg_from_type(ctx, type_ptr(elem));
    ir_emit_gep(bb, addr, base.value, idx, elem->size);
    u32 dst = alloc_vreg_from_type(ctx, elem);
    ir_emit_load(bb, dst, ir_operand_vreg(addr));
    return expr_result(ir_operand_vreg(dst), bb);
}

static ExprResult build_sizeof_expr(ASTSizeofExpr *se, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    (void) f;
    (void) ctx;
    return expr_result(ir_operand_imm((i64) se->size_value), bb);
}

static ExprResult build_sizeof_type(ASTSizeofType *st, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    (void) f;
    (void) ctx;
    return expr_result(ir_operand_imm((i64) st->size_value), bb);
}

static ExprResult build_string_literal_expr(ASTStringLiteral *sl, IrFunction *f, IrBlock *bb,
                                            FuncBuilder *ctx)
{
    (void) f;
    u32 idx = (u32) vec_size(ctx->mod->globals);
    IrGlobal *g = arena_alloc(ctx->mod->arena, sizeof(IrGlobal), sizeof(void *));
    size_t name_len = 16;
    char *name_buf = arena_alloc(ctx->mod->arena, name_len, 1);
    snprintf(name_buf, name_len, "__str_%u", idx);
    g->name = name_buf;
    g->type = type_array(type_char(), sl->length + 1);
    g->init_data = (const u8 *) sl->data;
    g->init_len = sl->length + 1;
    g->align = 1;
    g->section = IR_SECTION_RODATA;
    vec_push(ctx->mod->globals, g);
    return expr_result(ir_operand_global(idx), bb);
}

static ExprResult build_binary_expr(ASTBinaryExpr *be, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    if (be->op == BIN_LOG_AND || be->op == BIN_LOG_OR)
    {
        return build_short_circuit(be, f, bb, ctx);
    }
    if (be->op == BIN_ASSIGN)
    {
        return build_assign_expr(be, f, bb, ctx);
    }
    return build_arith_binop_expr(be, f, bb, ctx);
}

static ExprResult build_expr(ASTNode *node, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    switch (node->kind)
    {
        case AST_INT_LITERAL:
            return build_int_literal_expr(ast_as(ASTIntLiteral, node), f, bb, ctx);
        case AST_IDENT:
            return build_ident_expr(ast_as(ASTIdent, node), f, bb, ctx);
        case AST_BINARY_EXPR:
            return build_binary_expr(ast_as(ASTBinaryExpr, node), f, bb, ctx);
        case AST_UNARY_EXPR:
            return build_unary_expr(ast_as(ASTUnaryExpr, node), f, bb, ctx);
        case AST_CALL_EXPR:
            return build_call_expr(ast_as(ASTCallExpr, node), f, bb, ctx);
        case AST_TERNARY_EXPR:
            return build_ternary_expr(ast_as(ASTTernaryExpr, node), f, bb, ctx);
        case AST_SUBSCRIPT_EXPR:
            return build_subscript_expr(ast_as(ASTSubscriptExpr, node), f, bb, ctx);
        case AST_SIZEOF_EXPR:
            return build_sizeof_expr(ast_as(ASTSizeofExpr, node), f, bb, ctx);
        case AST_SIZEOF_TYPE:
            return build_sizeof_type(ast_as(ASTSizeofType, node), f, bb, ctx);
        case AST_STRING_LITERAL:
            return build_string_literal_expr(ast_as(ASTStringLiteral, node), f, bb, ctx);
        default:
            ir_error(node, "unsupported expression kind %s", ast_kind_name(node->kind));
            return expr_result(ir_operand_imm(0), bb);
    }
}

static void collect_labels(ASTNode *node, IrFunction *f, FuncBuilder *ctx)
{
    if (!node)
    {
        return;
    }

    switch (node->kind)
    {
        case AST_LABEL_STMT:
        {
            ASTLabelStmt *ls = ast_as(ASTLabelStmt, node);
            label_block(ctx, f, ls->label);
            collect_labels(ls->stmt, f, ctx);
            break;
        }
        case AST_COMPOUND_STMT:
        {
            ASTCompoundStmt *cs = ast_as(ASTCompoundStmt, node);
            size_t n = vec_size(cs->stmts);
            for (size_t i = 0; i < n; i++)
            {
                ASTNode *stmt = (ASTNode *) vec_get(cs->stmts, i);
                collect_labels(stmt, f, ctx);
            }
            break;
        }
        case AST_IF_STMT:
        {
            ASTIfStmt *is = ast_as(ASTIfStmt, node);
            collect_labels(is->then_branch, f, ctx);
            collect_labels(is->else_branch, f, ctx);
            break;
        }
        case AST_WHILE_STMT:
        {
            ASTWhileStmt *ws = ast_as(ASTWhileStmt, node);
            collect_labels(ws->body, f, ctx);
            break;
        }
        case AST_DO_WHILE_STMT:
        {
            ASTDoWhileStmt *ds = ast_as(ASTDoWhileStmt, node);
            collect_labels(ds->body, f, ctx);
            break;
        }
        case AST_FOR_STMT:
        {
            ASTForStmt *fs = ast_as(ASTForStmt, node);
            collect_labels(fs->body, f, ctx);
            break;
        }
        default:
            break;
    }
}

static IrBlock *label_block(FuncBuilder *ctx, IrFunction *f, const char *name)
{
    IrBlock *label_bb = (IrBlock *) strmap_get(ctx->goto_labels, name);
    if (!label_bb)
    {
        label_bb = ir_func_add_block(f, name);
        label_bb->is_loop_header = true;
        strmap_set(ctx->goto_labels, name, label_bb);
    }
    /* Label blocks are treated as merge points because backward gotos can add
       predecessor edges after this point, so PHIs must wait. */
    return label_bb;
}

static IrBlock *build_stmt_sequence(Vec *stmts, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    size_t n = vec_size(stmts);
    for (size_t i = 0; i < n; i++)
    {
        ASTNode *stmt = (ASTNode *) vec_get(stmts, i);
        if (is_terminated(bb))
        {
            IrBlock *unreach = new_block(f, "unreach");
            bb = build_stmt(stmt, f, unreach, ctx);
        }
        else
        {
            bb = build_stmt(stmt, f, bb, ctx);
        }
    }
    return bb;
}

static IrBlock *build_compound_stmt(ASTCompoundStmt *cs, IrFunction *f, IrBlock *bb,
                                    FuncBuilder *ctx)
{
    return build_stmt_sequence(cs->stmts, f, bb, ctx);
}

static void build_cond_branch(ASTNode *branch_stmt, IrFunction *f, IrBlock *branch_bb,
                              IrBlock *merge_bb, FuncBuilder *ctx)
{
    seal_block(ctx, branch_bb);

    IrBlock *end = branch_bb;
    if (branch_stmt)
    {
        end = build_stmt(branch_stmt, f, branch_bb, ctx);
    }
    if (!is_terminated(end))
    {
        jump(end, merge_bb);
    }
}

static IrBlock *build_if_stmt(ASTIfStmt *is, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    ExprResult cond = build_expr(is->cond, f, bb, ctx);
    bb = cond.block;

    IrBlock *then_bb = new_block(f, "then");
    IrBlock *else_bb = new_block(f, "else");
    IrBlock *merge_bb = new_block(f, "merge");

    cond_jump(bb, cond.value, then_bb, else_bb);

    build_cond_branch(is->then_branch, f, then_bb, merge_bb, ctx);
    build_cond_branch(is->else_branch, f, else_bb, merge_bb, ctx);
    seal_block(ctx, merge_bb);
    return merge_bb;
}

static IrBlock *build_loop_body(ASTNode *body_node, IrFunction *f, LoopBlocks *lb, FuncBuilder *ctx)
{
    LoopContext lc = {.continue_target = lb->latch, .exit = lb->exit};
    vec_push(ctx->loop_stack, &lc);
    IrBlock *end = build_stmt(body_node, f, lb->body, ctx);
    vec_pop(ctx->loop_stack);
    return end;
}

static IrBlock *finish_loop(ASTNode *cond_node, IrFunction *f, LoopBlocks *lb, FuncBuilder *ctx)
{
    IrBlock *h = lb->header;
    if (cond_node)
    {
        ExprResult c = build_expr(cond_node, f, h, ctx);
        h = c.block;
        ir_emit_brcond(h, c.value, lb->body->label, lb->exit->label);
    }
    else
    {
        ir_emit_br(h, lb->body->label);
    }
    declare_pred(lb->exit, h);
    seal_block(ctx, h);
    seal_block(ctx, lb->exit);
    return lb->exit;
}

static IrBlock *build_while_stmt(ASTWhileStmt *ws, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    IrBlock *header_bb = new_block(f, "while_header");
    IrBlock *body_bb = new_block(f, "while_body");
    IrBlock *exit_bb = new_block(f, "while_exit");
    header_bb->is_loop_header = true;

    LoopBlocks lb = {header_bb, body_bb, header_bb, exit_bb};

    jump(bb, lb.header);
    declare_pred(lb.body, lb.header);

    IrBlock *body_end = build_loop_body(ws->body, f, &lb, ctx);
    if (!is_terminated(body_end))
    {
        jump(body_end, lb.latch);
    }

    return finish_loop(ws->cond, f, &lb, ctx);
}

static IrBlock *build_do_while_stmt(ASTDoWhileStmt *ds, IrFunction *f, IrBlock *bb,
                                    FuncBuilder *ctx)
{
    IrBlock *body_bb = new_block(f, "do_body");
    IrBlock *header_bb = new_block(f, "do_header");
    IrBlock *exit_bb = new_block(f, "do_exit");
    header_bb->is_loop_header = true;

    LoopBlocks lb = {header_bb, body_bb, header_bb, exit_bb};

    jump(bb, lb.body);
    declare_pred(lb.body, lb.header);

    IrBlock *body_end = build_loop_body(ds->body, f, &lb, ctx);
    if (!is_terminated(body_end))
    {
        jump(body_end, lb.latch);
    }
    seal_block(ctx, lb.body);

    return finish_loop(ds->cond, f, &lb, ctx);
}

static IrBlock *build_for_stmt(ASTForStmt *fs, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    if (fs->init)
    {
        bb = build_stmt(fs->init, f, bb, ctx);
    }

    IrBlock *header_bb = new_block(f, "for_header");
    IrBlock *body_bb = new_block(f, "for_body");
    IrBlock *latch_bb = new_block(f, "for_latch");
    IrBlock *exit_bb = new_block(f, "for_exit");
    header_bb->is_loop_header = true;

    LoopBlocks lb = {header_bb, body_bb, latch_bb, exit_bb};

    jump(bb, lb.header);
    declare_pred(lb.body, lb.header);

    IrBlock *body_end = build_loop_body(fs->body, f, &lb, ctx);
    if (!is_terminated(body_end))
    {
        jump(body_end, lb.latch);
    }

    /* The latch is a merge point: seal it before the post-expression so
       post reads see sealed predecessors. */
    seal_block(ctx, lb.latch);

    if (fs->post)
    {
        ExprResult post = build_expr(fs->post, f, lb.latch, ctx);
        lb.latch = post.block;
    }
    if (!is_terminated(lb.latch))
    {
        jump(lb.latch, lb.header);
    }

    return finish_loop(fs->cond, f, &lb, ctx);
}

static IrBlock *build_break_stmt(ASTBreakStmt *bs, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    (void) bs;
    (void) f;
    ASSERT(vec_size(ctx->loop_stack) > 0);
    LoopContext *loop = (LoopContext *) vec_last(ctx->loop_stack);
    jump(bb, loop->exit);
    return bb;
}

static IrBlock *build_continue_stmt(ASTContinueStmt *cs, IrFunction *f, IrBlock *bb,
                                    FuncBuilder *ctx)
{
    (void) cs;
    (void) f;
    ASSERT(vec_size(ctx->loop_stack) > 0);
    LoopContext *loop = (LoopContext *) vec_last(ctx->loop_stack);
    jump(bb, loop->continue_target);
    return bb;
}

static IrBlock *build_goto_stmt(ASTGotoStmt *gs, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    (void) f;
    IrBlock *target = (IrBlock *) strmap_get(ctx->goto_labels, gs->label);
    ASSERT(target != NULL);
    jump(bb, target);
    return bb;
}

static IrBlock *build_label_stmt(ASTLabelStmt *ls, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    IrBlock *label_bb = label_block(ctx, f, ls->label);
    if (!is_terminated(bb))
    {
        jump(bb, label_bb);
    }
    return build_stmt(ls->stmt, f, label_bb, ctx);
}

static IrBlock *build_return_stmt(ASTReturnStmt *ret, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    if (ret->expr)
    {
        ExprResult val = build_expr(ret->expr, f, bb, ctx);
        ir_emit_ret(val.block, val.value);
        return val.block;
    }
    ir_emit_ret_void(bb);
    return bb;
}

static IrBlock *build_var_decl_stmt(ASTVarDecl *vd, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    strmap_set(ctx->var_types, vd->name, vd->type);
    if (type_is_array(vd->type))
    {
        /* Arrays are not SSA values: allocate a stack slot and keep the
           pointer as the variable's value. References decay to a pointer to
           the first element; subscripting GEPs off this base. */
        Type *ptr_type = type_decay(vd->type);
        u32 dst = alloc_vreg_from_type(ctx, ptr_type);
        ir_emit_alloca(bb, dst, vd->type->size);
        write_variable(ctx, vd->name, bb, ir_operand_vreg(dst));
        return bb;
    }
    IrOperand val = ir_operand_imm(0);
    if (vd->init)
    {
        ExprResult init = build_expr(vd->init, f, bb, ctx);
        bb = init.block;
        Type *rhs_type = node_type(vd->init);
        val = promote_to(ctx, bb, init.value, rhs_type, vd->type);
    }
    write_variable(ctx, vd->name, bb, val);
    return bb;
}

static IrBlock *build_expr_stmt(ASTExprStmt *es, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    ExprResult res = build_expr(es->expr, f, bb, ctx);
    return res.block;
}

static IrBlock *build_stmt(ASTNode *node, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    switch (node->kind)
    {
        case AST_RETURN_STMT:
            return build_return_stmt(ast_as(ASTReturnStmt, node), f, bb, ctx);
        case AST_VAR_DECL:
            return build_var_decl_stmt(ast_as(ASTVarDecl, node), f, bb, ctx);
        case AST_EXPR_STMT:
            return build_expr_stmt(ast_as(ASTExprStmt, node), f, bb, ctx);
        case AST_COMPOUND_STMT:
            return build_compound_stmt(ast_as(ASTCompoundStmt, node), f, bb, ctx);
        case AST_IF_STMT:
            return build_if_stmt(ast_as(ASTIfStmt, node), f, bb, ctx);
        case AST_WHILE_STMT:
            return build_while_stmt(ast_as(ASTWhileStmt, node), f, bb, ctx);
        case AST_DO_WHILE_STMT:
            return build_do_while_stmt(ast_as(ASTDoWhileStmt, node), f, bb, ctx);
        case AST_FOR_STMT:
            return build_for_stmt(ast_as(ASTForStmt, node), f, bb, ctx);
        case AST_BREAK_STMT:
            return build_break_stmt(ast_as(ASTBreakStmt, node), f, bb, ctx);
        case AST_CONTINUE_STMT:
            return build_continue_stmt(ast_as(ASTContinueStmt, node), f, bb, ctx);
        case AST_GOTO_STMT:
            return build_goto_stmt(ast_as(ASTGotoStmt, node), f, bb, ctx);
        case AST_LABEL_STMT:
            return build_label_stmt(ast_as(ASTLabelStmt, node), f, bb, ctx);
        default:
            ir_error(node, "unsupported statement kind %s", ast_kind_name(node->kind));
            return bb;
    }
}

static void setup_params(FuncBuilder *ctx, IrFunction *f, ASTFuncDef *ast, IrBlock *entry)
{
    size_t nparams = vec_size(ast->params);
    for (size_t i = 0; i < nparams; i++)
    {
        ASTVarDecl *param = ast_as(ASTVarDecl, (ASTNode *) vec_get(ast->params, i));
        u32 vreg = alloc_vreg_from_type(ctx, param->type);
        IrParam *p = arena_alloc(ctx->mod->arena, sizeof(IrParam), sizeof(void *));
        p->name = param->name;
        p->type = param->type;
        p->vreg = vreg;
        vec_push(f->params, p);
        strmap_set(ctx->var_types, param->name, param->type);
        write_variable(ctx, param->name, entry, ir_operand_vreg(vreg));
    }
}

static void seal_all_blocks(FuncBuilder *ctx, IrFunction *f)
{
    size_t nblocks = vec_size(f->blocks);
    for (size_t i = 0; i < nblocks; i++)
    {
        IrBlock *bb = (IrBlock *) vec_get(f->blocks, i);
        seal_block(ctx, bb);
    }
}

static void finish_func(IrFunction *f, IrBlock *exit, FuncBuilder *ctx)
{
    if (!is_terminated(exit))
    {
        if (f->ret_type->kind == TYPE_VOID)
        {
            ir_emit_ret_void(exit);
        }
        else
        {
            ir_emit_unreachable(exit);
        }
    }
    seal_all_blocks(ctx, f);
}

static bool build_func(ASTNode *ast, IrModule *mod, StrMap *func_types)
{
    if (ast->kind != AST_FUNC_DEF)
    {
        ir_error(ast, "expected function definition at top level");
        return false;
    }
    ASTFuncDef *func_ast = (ASTFuncDef *) ast;

    IrFunction *func = ir_module_add_func(mod, func_ast->name, func_ast->ret_type);
    IrBlock *entry = ir_func_add_block(func, "entry");

    if (func_ast->body->kind != AST_COMPOUND_STMT)
    {
        ir_error(func_ast->body, "expected compound statement as function body");
        return false;
    }

    FuncBuilder ctx = {mod, //
                       u64map_new(mod->arena),
                       vec_new(mod->arena),
                       strmap_new(mod->arena),
                       strmap_new(mod->arena),
                       func_types};

    /* Pre-create blocks for all labels so gotos can target them. */
    collect_labels(func_ast->body, func, &ctx);

    setup_params(&ctx, func, func_ast, entry);

    ASTCompoundStmt *body = (ASTCompoundStmt *) func_ast->body;
    entry = build_stmt_sequence(body->stmts, func, entry, &ctx);

    finish_func(func, entry, &ctx);

    return true;
}

IrModule *ir_build_module(ASTNode *ast, Arena *arena)
{
    if (ast->kind != AST_PROGRAM)
    {
        ir_error(ast, "expected program at top level");
        return NULL;
    }
    ASTProgram *prog = (ASTProgram *) ast;

    StrMap *func_types = strmap_new(arena);
    size_t ndecls = vec_size(prog->decls);
    for (size_t i = 0; i < ndecls; i++)
    {
        ASTNode *decl = (ASTNode *) vec_get(prog->decls, i);
        if (decl->kind == AST_FUNC_DEF)
        {
            ASTFuncDef *fn = ast_as(ASTFuncDef, decl);
            strmap_set(func_types, fn->name, fn->ret_type);
        }
    }

    IrModule *mod = ir_module_new(arena);
    for (size_t i = 0; i < ndecls; i++)
    {
        ASTNode *decl = (ASTNode *) vec_get(prog->decls, i);
        if (!build_func(decl, mod, func_types))
        {
            return NULL;
        }
    }
    return mod;
}
