#include "ir_builder.h"
#include "util/hashmap.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct BlockState BlockState;
struct BlockState
{
    StrMap *locals;       /* name -> IrOperand* */
    Vec *incomplete_phis; /* Vec<IncompletePhi*> */
};

typedef struct IncompletePhi IncompletePhi;
struct IncompletePhi
{
    const char *name;
    u32 dst_vreg;
};

typedef struct ExprResult ExprResult;
struct ExprResult
{
    IrOperand val;
    IrBlock *bb;
};

typedef struct LoopContext LoopContext;
struct LoopContext
{
    IrBlock *header;
    IrBlock *exit;
};

typedef struct BuilderCtx BuilderCtx;
struct BuilderCtx
{
    IrModule *mod;
    U64Map *block_states; /* (u64)IrBlock* -> BlockState* */
    Vec *loop_stack;      /* Vec<LoopContext*> */
    StrMap *goto_labels;  /* label name -> IrBlock* */
};

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

static IrOperand *make_operand_ptr(BuilderCtx *ctx, IrOperand op)
{
    IrOperand *p = arena_alloc(ctx->mod->arena, sizeof(IrOperand), sizeof(IrOperand));
    *p = op;
    return p;
}

static BlockState *get_block_state(BuilderCtx *ctx, IrBlock *bb)
{
    BlockState *bs = u64map_get(ctx->block_states, (u64) (uintptr_t) bb);
    if (!bs)
    {
        bs = arena_alloc(ctx->mod->arena, sizeof(BlockState), sizeof(void *));
        bs->locals = strmap_new(ctx->mod->arena);
        bs->incomplete_phis = vec_new(ctx->mod->arena);
        u64map_set(ctx->block_states, (u64) (uintptr_t) bb, bs);
    }
    return bs;
}

/* Create a new block with a unique label, avoiding collisions when multiple
   control-flow constructs appear in the same function. */
static IrBlock *add_unique_block(IrFunction *f, const char *prefix)
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

static IrOperand read_variable(BuilderCtx *ctx, const char *name, IrBlock *bb);
static ExprResult build_expr(ASTNode *node, IrFunction *f, IrBlock *bb, BuilderCtx *ctx);
static void seal_block(BuilderCtx *ctx, IrBlock *bb);

/* Emit a phi and move it to the front of the block so it dominates every other
   instruction. */
static IrInstr *emit_phi_at_start(IrBlock *bb, u32 dst, u32 nentries)
{
    IrInstr *phi = ir_emit_phi(bb, dst, nentries);
    IrInstr *last = (IrInstr *) vec_pop(bb->instrs);
    ASSERT(last == phi);
    vec_insert(bb->instrs, 0, phi);
    return phi;
}

/* Multiple predecessors: value must come through a PHI, one entry per pred. */
static IrOperand build_phi(BuilderCtx *ctx, const char *name, IrBlock *bb)
{
    u32 nentries = (u32) vec_size(bb->preds);
    u32 dst = ir_alloc_vreg(ctx->mod, 4);
    IrInstr *phi = emit_phi_at_start(bb, dst, nentries);
    for (u32 e = 0; e < nentries; e++)
    {
        IrBlock *pred = (IrBlock *) vec_get(bb->preds, e);
        IrOperand pval = read_variable(ctx, name, pred);
        ir_phi_add_entry(phi, pval, pred);
    }
    return ir_operand_vreg(dst);
}

/* Fill a placeholder phi created while the block was unsealed. */
static void insert_phi(BuilderCtx *ctx, IrBlock *bb, const char *name, u32 dst)
{
    u32 nentries = (u32) vec_size(bb->preds);
    IrInstr *phi = emit_phi_at_start(bb, dst, nentries);
    for (u32 e = 0; e < nentries; e++)
    {
        IrBlock *pred = (IrBlock *) vec_get(bb->preds, e);
        IrOperand pval = read_variable(ctx, name, pred);
        ir_phi_add_entry(phi, pval, pred);
    }
}

static void seal_block(BuilderCtx *ctx, IrBlock *bb)
{
    if (bb->sealed)
    {
        return;
    }
    bb->sealed = true;

    BlockState *bs = get_block_state(ctx, bb);
    Vec *phis = bs->incomplete_phis;
    size_t n = vec_size(phis);
    for (size_t i = 0; i < n; i++)
    {
        IncompletePhi *ip = (IncompletePhi *) vec_get(phis, i);
        insert_phi(ctx, bb, ip->name, ip->dst_vreg);
    }
}

static IrOperand read_variable_recursive(BuilderCtx *ctx, const char *name, IrBlock *bb)
{
    BlockState *bs = get_block_state(ctx, bb);
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
        u32 dst = ir_alloc_vreg(ctx->mod, 4);
        IncompletePhi *ip = arena_alloc(ctx->mod->arena, sizeof(IncompletePhi), sizeof(void *));
        ip->name = name;
        ip->dst_vreg = dst;
        vec_push(bs->incomplete_phis, ip);
        val = ir_operand_vreg(dst);
    }
    strmap_set(bs->locals, name, make_operand_ptr(ctx, val));
    return val;
}

static IrOperand read_variable(BuilderCtx *ctx, const char *name, IrBlock *bb)
{
    BlockState *bs = get_block_state(ctx, bb);
    IrOperand *p = strmap_get(bs->locals, name);
    if (p)
    {
        return *p;
    }
    return read_variable_recursive(ctx, name, bb);
}

static void write_variable(BuilderCtx *ctx, const char *name, IrBlock *bb, IrOperand val)
{
    BlockState *bs = get_block_state(ctx, bb);
    strmap_set(bs->locals, name, make_operand_ptr(ctx, val));
}

static ExprResult expr_result(IrOperand val, IrBlock *bb)
{
    ExprResult r = {val, bb};
    return r;
}

static const IrOpcode icmp_opcodes[] = {
    [BIN_EQ] = OP_ICMP_EQ,
    [BIN_NE] = OP_ICMP_NE,
    [BIN_LT] = OP_ICMP_SLT,
    [BIN_GT] = OP_ICMP_SGT,
    [BIN_LE] = OP_ICMP_SLE,
    [BIN_GE] = OP_ICMP_SGE,
};

static const IrOpcode arith_opcodes[] = {
    [BIN_ADD] = OP_ADD,
    [BIN_SUB] = OP_SUB,
    [BIN_MUL] = OP_MUL,
    [BIN_DIV] = OP_SDIV,
    [BIN_REM] = OP_SREM,
    [BIN_AND] = OP_AND,
    [BIN_OR] = OP_OR,
    [BIN_XOR] = OP_XOR,
    [BIN_SHL] = OP_SHL,
    [BIN_SHR] = OP_ASHR,
};

static void emit_arith_binop(IrBlock *bb, u32 dst, BinOpKind op, IrOperand left, IrOperand right,
                             ASTNode *node)
{
    IrOpcode code = arith_opcodes[op];
    if (code == 0)
    {
        ir_error(node, "unsupported binary operator");
        return;
    }
    ir_emit_binop(bb, code, dst, left, right);
}

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

static IrOperand build_assignment(IrBlock *bb, ASTNode *node, IrOperand value, BuilderCtx *ctx)
{
    ASTBinaryExpr *assign = ast_as(ASTBinaryExpr, node);
    if (assign->left->kind != AST_IDENT)
    {
        ir_error(node, "assignment target must be an identifier");
        return ir_operand_imm(0);
    }
    ASTIdent *target = ast_as(ASTIdent, assign->left);
    write_variable(ctx, target->name, bb, value);
    return value;
}

static ExprResult build_short_circuit(ASTNode *left_node, ASTNode *right_node, bool is_or,
                                      IrFunction *f, IrBlock *bb, BuilderCtx *ctx)
{
    ExprResult left = build_expr(left_node, f, bb, ctx);
    if (!left.bb)
    {
        return left;
    }

    char name[16];
    IrBlock *true_bb = add_unique_block(f, is_or ? "lor_true" : "land_true");
    IrBlock *false_bb = add_unique_block(f, is_or ? "lor_false" : "land_false");
    snprintf(name, sizeof(name), "%s_rhs", is_or ? "lor" : "land");
    IrBlock *right_bb = add_unique_block(f, name);
    snprintf(name, sizeof(name), "%s_merge", is_or ? "lor" : "land");
    IrBlock *merge_bb = add_unique_block(f, name);

    if (is_or)
    {
        ir_emit_brcond(left.bb, left.val, true_bb->label, right_bb->label);
        vec_push(true_bb->preds, left.bb);
        vec_push(right_bb->preds, left.bb);
    }
    else
    {
        ir_emit_brcond(left.bb, left.val, right_bb->label, false_bb->label);
        vec_push(right_bb->preds, left.bb);
        vec_push(false_bb->preds, left.bb);
    }

    ExprResult right = build_expr(right_node, f, right_bb, ctx);
    if (!right.bb)
    {
        return right;
    }
    ir_emit_brcond(right.bb, right.val, true_bb->label, false_bb->label);
    vec_push(true_bb->preds, right.bb);
    vec_push(false_bb->preds, right.bb);

    ir_emit_br(true_bb, merge_bb->label);
    ir_emit_br(false_bb, merge_bb->label);
    vec_push(merge_bb->preds, true_bb);
    vec_push(merge_bb->preds, false_bb);

    /* Seal the short-circuit blocks before the merge so that values flowing
       into the merge come from sealed predecessors. */
    seal_block(ctx, left.bb);
    seal_block(ctx, right_bb);
    seal_block(ctx, false_bb);
    seal_block(ctx, true_bb);

    /* The merge block is a real merge point: seal it so future variable reads
       produce proper PHIs, and insert the logical-result PHI at the top. */
    merge_bb->sealed = true;

    u32 dst = ir_alloc_vreg(ctx->mod, 4);
    IrInstr *phi = emit_phi_at_start(merge_bb, dst, 2);
    ir_phi_add_entry(phi, ir_operand_imm(1), true_bb);
    ir_phi_add_entry(phi, ir_operand_imm(0), false_bb);

    return expr_result(ir_operand_vreg(dst), merge_bb);
}

static ExprResult build_ternary(ASTNode *cond_node, ASTNode *then_node, ASTNode *else_node,
                                IrFunction *f, IrBlock *bb, BuilderCtx *ctx)
{
    ExprResult cond = build_expr(cond_node, f, bb, ctx);
    if (!cond.bb)
    {
        return cond;
    }

    IrBlock *then_bb = add_unique_block(f, "tern_then");
    IrBlock *else_bb = add_unique_block(f, "tern_else");
    IrBlock *merge_bb = add_unique_block(f, "tern_merge");

    ir_emit_brcond(cond.bb, cond.val, then_bb->label, else_bb->label);
    vec_push(then_bb->preds, cond.bb);
    vec_push(else_bb->preds, cond.bb);

    ExprResult then_val = build_expr(then_node, f, then_bb, ctx);
    if (!then_val.bb)
    {
        return then_val;
    }
    if (!is_terminated(then_val.bb))
    {
        ir_emit_br(then_val.bb, merge_bb->label);
        vec_push(merge_bb->preds, then_val.bb);
    }

    ExprResult else_val = build_expr(else_node, f, else_bb, ctx);
    if (!else_val.bb)
    {
        return else_val;
    }
    if (!is_terminated(else_val.bb))
    {
        ir_emit_br(else_val.bb, merge_bb->label);
        vec_push(merge_bb->preds, else_val.bb);
    }

    u32 dst = ir_alloc_vreg(ctx->mod, 4);
    IrInstr *phi = emit_phi_at_start(merge_bb, dst, 2);
    ir_phi_add_entry(phi, then_val.val, then_val.bb);
    ir_phi_add_entry(phi, else_val.val, else_val.bb);
    merge_bb->sealed = true;

    return expr_result(ir_operand_vreg(dst), merge_bb);
}

static ExprResult build_expr(ASTNode *node, IrFunction *f, IrBlock *bb, BuilderCtx *ctx)
{
    switch (node->kind)
    {
        case AST_INT_LITERAL:
        {
            ASTIntLiteral *lit = ast_as(ASTIntLiteral, node);
            return expr_result(ir_operand_imm(lit->value), bb);
        }
        case AST_IDENT:
        {
            ASTIdent *id = ast_as(ASTIdent, node);
            return expr_result(read_variable(ctx, id->name, bb), bb);
        }
        case AST_BINARY_EXPR:
        {
            ASTBinaryExpr *be = ast_as(ASTBinaryExpr, node);
            if (be->op == BIN_LOG_AND)
            {
                return build_short_circuit(be->left, be->right, false, f, bb, ctx);
            }
            if (be->op == BIN_LOG_OR)
            {
                return build_short_circuit(be->left, be->right, true, f, bb, ctx);
            }
            ExprResult left = build_expr(be->left, f, bb, ctx);
            bb = left.bb;
            if (be->op == BIN_ASSIGN)
            {
                ExprResult right = build_expr(be->right, f, bb, ctx);
                bb = right.bb;
                IrOperand val = build_assignment(bb, node, right.val, ctx);
                return expr_result(val, bb);
            }
            ExprResult right = build_expr(be->right, f, bb, ctx);
            bb = right.bb;
            u32 dst = ir_alloc_vreg(ctx->mod, 4);
            switch (be->op)
            {
                case BIN_EQ:
                case BIN_NE:
                case BIN_LT:
                case BIN_GT:
                case BIN_LE:
                case BIN_GE:
                    ir_emit_binop(bb, icmp_opcodes[be->op], dst, left.val, right.val);
                    break;
                default:
                    emit_arith_binop(bb, dst, be->op, left.val, right.val, node);
                    break;
            }
            return expr_result(ir_operand_vreg(dst), bb);
        }
        case AST_UNARY_EXPR:
        {
            ASTUnaryExpr *ue = ast_as(ASTUnaryExpr, node);
            ExprResult src = build_expr(ue->operand, f, bb, ctx);
            bb = src.bb;
            u32 dst = ir_alloc_vreg(ctx->mod, 4);
            if (ue->op == UN_LOG_NOT)
            {
                ir_emit_binop(bb, OP_ICMP_EQ, dst, src.val, ir_operand_imm(0));
            }
            else
            {
                emit_unary_op(bb, dst, ue->op, src.val, node);
            }
            return expr_result(ir_operand_vreg(dst), bb);
        }
        case AST_CALL_EXPR:
        {
            ASTCallExpr *ce = ast_as(ASTCallExpr, node);
            u32 nargs = (u32) vec_size(ce->args);
            IrOperand *args = arena_alloc(ctx->mod->arena, nargs * sizeof(IrOperand), sizeof(IrOperand));
            for (u32 i = 0; i < nargs; i++)
            {
                ASTNode *arg = (ASTNode *) vec_get(ce->args, i);
                ExprResult arg_res = build_expr(arg, f, bb, ctx);
                args[i] = arg_res.val;
                bb = arg_res.bb;
            }
            u32 dst = ir_alloc_vreg(ctx->mod, 4);
            ir_emit_call(bb, dst, ce->callee, nargs, args);
            return expr_result(ir_operand_vreg(dst), bb);
        }
        case AST_TERNARY_EXPR:
        {
            ASTTernaryExpr *te = ast_as(ASTTernaryExpr, node);
            return build_ternary(te->cond, te->then_expr, te->else_expr, f, bb, ctx);
        }
        default:
            ir_error(node, "unsupported expression kind %s", ast_kind_name(node->kind));
            return expr_result(ir_operand_imm(0), bb);
    }
}

static IrBlock *build_stmt(ASTNode *node, IrFunction *f, IrBlock *bb, BuilderCtx *ctx);

static void collect_labels(ASTNode *node, IrFunction *f, BuilderCtx *ctx)
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
            if (!strmap_get(ctx->goto_labels, ls->label))
            {
                IrBlock *label_bb = ir_func_add_block(f, ls->label);
                label_bb->is_loop_header = true;
                strmap_set(ctx->goto_labels, ls->label, label_bb);
            }
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

static IrBlock *build_stmt_sequence(Vec *stmts, IrFunction *f, IrBlock *bb, BuilderCtx *ctx)
{
    size_t n = vec_size(stmts);
    for (size_t i = 0; i < n; i++)
    {
        ASTNode *stmt = (ASTNode *) vec_get(stmts, i);
        if (is_terminated(bb))
        {
            /* Statements after a terminator are unreachable. Place them in a
               fresh block so they cannot pollute the value map of reachable
               blocks. */
            IrBlock *unreach = add_unique_block(f, "unreach");
            bb = build_stmt(stmt, f, unreach, ctx);
        }
        else
        {
            bb = build_stmt(stmt, f, bb, ctx);
        }
    }
    return bb;
}

static IrBlock *build_compound_stmt(ASTCompoundStmt *cs, IrFunction *f, IrBlock *bb, BuilderCtx *ctx)
{
    return build_stmt_sequence(cs->stmts, f, bb, ctx);
}

/* Build one if/else branch: wire the entry, build the statement (if any), and
   fall through to the merge block unless the branch already terminates. */
static void build_branch(ASTNode *branch_stmt, IrFunction *f, IrBlock *cond_bb, IrBlock *branch_bb,
                         IrBlock *merge_bb, BuilderCtx *ctx)
{
    vec_push(branch_bb->preds, cond_bb);
    seal_block(ctx, branch_bb);

    IrBlock *end = branch_bb;
    if (branch_stmt)
    {
        end = build_stmt(branch_stmt, f, branch_bb, ctx);
    }
    if (!is_terminated(end))
    {
        ir_emit_br(end, merge_bb->label);
        vec_push(merge_bb->preds, end);
    }
}

static IrBlock *build_if_stmt(ASTIfStmt *is, IrFunction *f, IrBlock *bb, BuilderCtx *ctx)
{
    ExprResult cond = build_expr(is->cond, f, bb, ctx);
    bb = cond.bb;

    IrBlock *then_bb = add_unique_block(f, "then");
    IrBlock *else_bb = add_unique_block(f, "else");
    IrBlock *merge_bb = add_unique_block(f, "merge");

    ir_emit_brcond(bb, cond.val, then_bb->label, else_bb->label);

    build_branch(is->then_branch, f, bb, then_bb, merge_bb, ctx);
    build_branch(is->else_branch, f, bb, else_bb, merge_bb, ctx);
    seal_block(ctx, merge_bb);
    return merge_bb;
}

static IrBlock *build_while_stmt(ASTWhileStmt *ws, IrFunction *f, IrBlock *bb, BuilderCtx *ctx)
{
    IrBlock *header_bb = add_unique_block(f, "while_header");
    IrBlock *body_bb = add_unique_block(f, "while_body");
    IrBlock *exit_bb = add_unique_block(f, "while_exit");
    header_bb->is_loop_header = true;

    ir_emit_br(bb, header_bb->label);
    vec_push(header_bb->preds, bb);
    vec_push(body_bb->preds, header_bb);

    LoopContext loop = {header_bb, exit_bb};
    vec_push(ctx->loop_stack, &loop);
    IrBlock *body_end = build_stmt(ws->body, f, body_bb, ctx);
    vec_pop(ctx->loop_stack);

    if (!is_terminated(body_end))
    {
        ir_emit_br(body_end, header_bb->label);
        vec_push(header_bb->preds, body_end);
    }

    ExprResult cond = build_expr(ws->cond, f, header_bb, ctx);
    header_bb = cond.bb;
    ir_emit_brcond(header_bb, cond.val, body_bb->label, exit_bb->label);
    vec_push(exit_bb->preds, header_bb);

    seal_block(ctx, header_bb);
    seal_block(ctx, exit_bb);
    return exit_bb;
}

static IrBlock *build_do_while_stmt(ASTDoWhileStmt *ds, IrFunction *f, IrBlock *bb, BuilderCtx *ctx)
{
    IrBlock *body_bb = add_unique_block(f, "do_body");
    IrBlock *header_bb = add_unique_block(f, "do_header");
    IrBlock *exit_bb = add_unique_block(f, "do_exit");
    header_bb->is_loop_header = true;

    ir_emit_br(bb, body_bb->label);
    vec_push(body_bb->preds, bb);
    vec_push(body_bb->preds, header_bb);

    LoopContext loop = {header_bb, exit_bb};
    vec_push(ctx->loop_stack, &loop);
    IrBlock *body_end = build_stmt(ds->body, f, body_bb, ctx);
    vec_pop(ctx->loop_stack);

    if (!is_terminated(body_end))
    {
        ir_emit_br(body_end, header_bb->label);
        vec_push(header_bb->preds, body_end);
    }

    seal_block(ctx, body_bb);

    ExprResult cond = build_expr(ds->cond, f, header_bb, ctx);
    header_bb = cond.bb;
    ir_emit_brcond(header_bb, cond.val, body_bb->label, exit_bb->label);
    vec_push(exit_bb->preds, header_bb);

    seal_block(ctx, header_bb);
    seal_block(ctx, exit_bb);
    return exit_bb;
}

static IrBlock *build_for_stmt(ASTForStmt *fs, IrFunction *f, IrBlock *bb, BuilderCtx *ctx)
{
    if (fs->init)
    {
        bb = build_stmt(fs->init, f, bb, ctx);
    }

    IrBlock *header_bb = add_unique_block(f, "for_header");
    IrBlock *body_bb = add_unique_block(f, "for_body");
    IrBlock *latch_bb = add_unique_block(f, "for_latch");
    IrBlock *exit_bb = add_unique_block(f, "for_exit");
    header_bb->is_loop_header = true;

    ir_emit_br(bb, header_bb->label);
    vec_push(header_bb->preds, bb);
    vec_push(body_bb->preds, header_bb);

    LoopContext loop = {latch_bb, exit_bb};
    vec_push(ctx->loop_stack, &loop);
    IrBlock *body_end = build_stmt(fs->body, f, body_bb, ctx);
    vec_pop(ctx->loop_stack);

    if (!is_terminated(body_end))
    {
        ir_emit_br(body_end, latch_bb->label);
        vec_push(latch_bb->preds, body_end);
    }

    /* The latch is a merge point: it is entered by the body's fall-through and
       by `continue`. All its predecessors are known now, so seal it before the
       post-expression reads loop variables. */
    seal_block(ctx, latch_bb);

    if (fs->post)
    {
        ExprResult post = build_expr(fs->post, f, latch_bb, ctx);
        latch_bb = post.bb;
    }
    if (!is_terminated(latch_bb))
    {
        ir_emit_br(latch_bb, header_bb->label);
        vec_push(header_bb->preds, latch_bb);
    }

    ExprResult cond;
    if (fs->cond)
    {
        cond = build_expr(fs->cond, f, header_bb, ctx);
        header_bb = cond.bb;
        ir_emit_brcond(header_bb, cond.val, body_bb->label, exit_bb->label);
    }
    else
    {
        ir_emit_br(header_bb, body_bb->label);
    }
    vec_push(exit_bb->preds, header_bb);

    seal_block(ctx, header_bb);
    seal_block(ctx, exit_bb);
    return exit_bb;
}

static IrBlock *build_break_stmt(ASTBreakStmt *bs, IrFunction *f, IrBlock *bb, BuilderCtx *ctx)
{
    (void) bs;
    (void) f;
    ASSERT(vec_size(ctx->loop_stack) > 0);
    LoopContext *loop = (LoopContext *) vec_last(ctx->loop_stack);
    ir_emit_br(bb, loop->exit->label);
    vec_push(loop->exit->preds, bb);
    return bb;
}

static IrBlock *build_continue_stmt(ASTContinueStmt *cs, IrFunction *f, IrBlock *bb, BuilderCtx *ctx)
{
    (void) cs;
    (void) f;
    ASSERT(vec_size(ctx->loop_stack) > 0);
    LoopContext *loop = (LoopContext *) vec_last(ctx->loop_stack);
    ir_emit_br(bb, loop->header->label);
    vec_push(loop->header->preds, bb);
    return bb;
}

static IrBlock *build_goto_stmt(ASTGotoStmt *gs, IrFunction *f, IrBlock *bb, BuilderCtx *ctx)
{
    (void) f;
    IrBlock *target = (IrBlock *) strmap_get(ctx->goto_labels, gs->label);
    ASSERT(target != NULL);
    ir_emit_br(bb, target->label);
    vec_push(target->preds, bb);
    return bb;
}

static IrBlock *build_label_stmt(ASTLabelStmt *ls, IrFunction *f, IrBlock *bb, BuilderCtx *ctx)
{
    IrBlock *label_bb = (IrBlock *) strmap_get(ctx->goto_labels, ls->label);
    if (!label_bb)
    {
        label_bb = ir_func_add_block(f, ls->label);
        label_bb->is_loop_header = true;
        strmap_set(ctx->goto_labels, ls->label, label_bb);
    }
    /* If the current block is not terminated, fall through to the label block. */
    if (!is_terminated(bb))
    {
        ir_emit_br(bb, label_bb->label);
        vec_push(label_bb->preds, bb);
    }
    /* Label blocks are deferred until the end of the function: a backward goto
       can add a predecessor edge after this point, so PHIs must wait for all
       predecessors to be known. */
    return build_stmt(ls->stmt, f, label_bb, ctx);
}

static IrBlock *build_stmt(ASTNode *node, IrFunction *f, IrBlock *bb, BuilderCtx *ctx)
{
    switch (node->kind)
    {
        case AST_RETURN_STMT:
        {
            ASTReturnStmt *ret = ast_as(ASTReturnStmt, node);
            if (ret->expr)
            {
                ExprResult val = build_expr(ret->expr, f, bb, ctx);
                ir_emit_ret(val.bb, val.val);
            }
            else
            {
                ir_emit_ret_void(bb);
            }
            return bb;
        }
        case AST_VAR_DECL:
        {
            ASTVarDecl *vd = ast_as(ASTVarDecl, node);
            IrOperand val = ir_operand_imm(0);
            if (vd->init)
            {
                ExprResult init = build_expr(vd->init, f, bb, ctx);
                bb = init.bb;
                val = init.val;
            }
            write_variable(ctx, vd->name, bb, val);
            return bb;
        }
        case AST_EXPR_STMT:
        {
            ASTExprStmt *es = ast_as(ASTExprStmt, node);
            ExprResult res = build_expr(es->expr, f, bb, ctx);
            return res.bb;
        }
        case AST_COMPOUND_STMT:
        {
            ASTCompoundStmt *cs = ast_as(ASTCompoundStmt, node);
            return build_compound_stmt(cs, f, bb, ctx);
        }
        case AST_IF_STMT:
        {
            ASTIfStmt *is = ast_as(ASTIfStmt, node);
            return build_if_stmt(is, f, bb, ctx);
        }
        case AST_WHILE_STMT:
        {
            ASTWhileStmt *ws = ast_as(ASTWhileStmt, node);
            return build_while_stmt(ws, f, bb, ctx);
        }
        case AST_DO_WHILE_STMT:
        {
            ASTDoWhileStmt *ds = ast_as(ASTDoWhileStmt, node);
            return build_do_while_stmt(ds, f, bb, ctx);
        }
        case AST_FOR_STMT:
        {
            ASTForStmt *fs = ast_as(ASTForStmt, node);
            return build_for_stmt(fs, f, bb, ctx);
        }
        case AST_BREAK_STMT:
        {
            ASTBreakStmt *bs = ast_as(ASTBreakStmt, node);
            return build_break_stmt(bs, f, bb, ctx);
        }
        case AST_CONTINUE_STMT:
        {
            ASTContinueStmt *cs = ast_as(ASTContinueStmt, node);
            return build_continue_stmt(cs, f, bb, ctx);
        }
        case AST_GOTO_STMT:
        {
            ASTGotoStmt *gs = ast_as(ASTGotoStmt, node);
            return build_goto_stmt(gs, f, bb, ctx);
        }
        case AST_LABEL_STMT:
        {
            ASTLabelStmt *ls = ast_as(ASTLabelStmt, node);
            return build_label_stmt(ls, f, bb, ctx);
        }
        default:
            ir_error(node, "unsupported statement kind %s", ast_kind_name(node->kind));
            return bb;
    }
}

static bool build_func(ASTNode *ast, IrModule *mod)
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

    BuilderCtx ctx = {mod, u64map_new(mod->arena), vec_new(mod->arena), strmap_new(mod->arena)};

    /* Pre-create blocks for all labels so gotos can target them. */
    collect_labels(func_ast->body, func, &ctx);

    /* Allocate vregs for parameters and record them */
    size_t nparams = vec_size(func_ast->params);
    for (size_t i = 0; i < nparams; i++)
    {
        ASTVarDecl *param = ast_as(ASTVarDecl, (ASTNode *) vec_get(func_ast->params, i));
        u32 vreg = ir_alloc_vreg(mod, 4);
        IrParam *p = arena_alloc(mod->arena, sizeof(IrParam), sizeof(void *));
        p->name = param->name;
        p->type = param->type;
        p->vreg = vreg;
        vec_push(func->params, p);
        write_variable(&ctx, param->name, entry, ir_operand_vreg(vreg));
    }

    ASTCompoundStmt *body = (ASTCompoundStmt *) func_ast->body;
    entry = build_stmt_sequence(body->stmts, func, entry, &ctx);

    /* Ensure block ends with a terminator */
    if (!is_terminated(entry))
    {
        if (func_ast->ret_type->kind == TYPE_VOID)
        {
            ir_emit_ret_void(entry);
        }
        else
        {
            ir_emit_unreachable(entry);
        }
    }

    /* Seal every block that is still unsealed. Label blocks are deliberately
       left unsealed during construction so that backward-goto predecessor edges
       are known before their PHIs are built. */
    size_t nblocks = vec_size(func->blocks);
    for (size_t i = 0; i < nblocks; i++)
    {
        IrBlock *bb = (IrBlock *) vec_get(func->blocks, i);
        seal_block(&ctx, bb);
    }

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

    IrModule *mod = ir_module_new(arena);
    size_t ndecls = vec_size(prog->decls);
    for (size_t i = 0; i < ndecls; i++)
    {
        ASTNode *decl = (ASTNode *) vec_get(prog->decls, i);
        if (!build_func(decl, mod))
        {
            return NULL;
        }
    }
    return mod;
}
