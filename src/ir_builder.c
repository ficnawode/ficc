#include "ir_builder.h"
#include "util/hashmap.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct BlockState BlockState;
struct BlockState
{
    StrMap *locals;       /* name -> Operand* */
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
    Operand val;
    Block *bb;
};

typedef struct LoopContext LoopContext;
struct LoopContext
{
    Block *header;
    Block *exit;
};

typedef struct BuilderCtx BuilderCtx;
struct BuilderCtx
{
    Module *mod;
    U64Map *block_states; /* (u64)Block* -> BlockState* */
    Vec *loop_stack;      /* Vec<LoopContext*> */
    StrMap *goto_labels;  /* label name -> Block* */
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

static Operand *make_operand_ptr(BuilderCtx *ctx, Operand op)
{
    Operand *p = arena_alloc(ctx->mod->arena, sizeof(Operand), sizeof(Operand));
    *p = op;
    return p;
}

static BlockState *get_block_state(BuilderCtx *ctx, Block *bb)
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
static Block *add_unique_block(Function *f, const char *prefix)
{
    size_t idx = vec_size(f->blocks);
    size_t len = strlen(prefix);
    char *buf = arena_alloc(f->arena, len + 32, sizeof(char));
    snprintf(buf, len + 32, "%s_%zu", prefix, idx);
    return ir_func_add_block(f, buf);
}

static bool is_terminated(Block *bb)
{
    size_t n = vec_size(bb->instrs);
    if (n == 0)
    {
        return false;
    }
    Instr *last = (Instr *) vec_get(bb->instrs, n - 1);
    return last->opcode == OP_RET || last->opcode == OP_UNREACHABLE || last->opcode == OP_BR ||
           last->opcode == OP_BRCOND;
}

static Operand read_variable(BuilderCtx *ctx, const char *name, Block *bb);
static ExprResult build_expr(ASTNode *node, Function *f, Block *bb, BuilderCtx *ctx);
static void seal_block(BuilderCtx *ctx, Block *bb);

/* Multiple predecessors: value must come through a PHI, one entry per pred.
   The PHI is inserted at the beginning of the block so it dominates all other
   instructions. */
static Operand build_phi(BuilderCtx *ctx, const char *name, Block *bb)
{
    u32 nentries = (u32) vec_size(bb->preds);
    u32 dst = ir_alloc_vreg(ctx->mod, 4);
    Instr *phi = ir_emit_phi(bb, dst, nentries);
    for (u32 e = 0; e < nentries; e++)
    {
        Block *pred = (Block *) vec_get(bb->preds, e);
        Operand pval = read_variable(ctx, name, pred);
        ir_phi_add_entry(phi, pval, pred);
    }
    Instr *last = (Instr *) vec_pop(bb->instrs);
    ASSERT(last == phi);
    vec_insert(bb->instrs, 0, phi);
    return ir_operand_vreg(dst);
}

/* Create and insert a filled phi at the beginning of a block. */
static void insert_phi(BuilderCtx *ctx, Block *bb, const char *name, u32 dst)
{
    u32 nentries = (u32) vec_size(bb->preds);
    Instr *phi = ir_emit_phi(bb, dst, nentries);
    for (u32 e = 0; e < nentries; e++)
    {
        Block *pred = (Block *) vec_get(bb->preds, e);
        Operand pval = read_variable(ctx, name, pred);
        ir_phi_add_entry(phi, pval, pred);
    }
    /* ir_emit_phi appended; move the phi to the beginning of the block. */
    Instr *last = (Instr *) vec_pop(bb->instrs);
    ASSERT(last == phi);
    vec_insert(bb->instrs, 0, phi);
}

static void seal_block(BuilderCtx *ctx, Block *bb)
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

static Operand read_variable_recursive(BuilderCtx *ctx, const char *name, Block *bb)
{
    BlockState *bs = get_block_state(ctx, bb);
    Operand val;
    if (vec_size(bb->preds) == 0)
    {
        /* No predecessors: undefined (semantic already checked) */
        val = ir_operand_imm(0);
    }
    else if (vec_size(bb->preds) == 1 && !bb->is_loop_header)
    {
        /* Single predecessor and not a loop header: no PHI needed. Read the value
           from the predecessor. For an unsealed predecessor this creates a
           placeholder that will be filled when that block is sealed. */
        Block *pred = (Block *) vec_get(bb->preds, 0);
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

static Operand read_variable(BuilderCtx *ctx, const char *name, Block *bb)
{
    BlockState *bs = get_block_state(ctx, bb);
    Operand *p = strmap_get(bs->locals, name);
    if (p)
    {
        return *p;
    }
    return read_variable_recursive(ctx, name, bb);
}

static void write_variable(BuilderCtx *ctx, const char *name, Block *bb, Operand val)
{
    BlockState *bs = get_block_state(ctx, bb);
    strmap_set(bs->locals, name, make_operand_ptr(ctx, val));
}

static ExprResult expr_result(Operand val, Block *bb)
{
    ExprResult r = {val, bb};
    return r;
}

static IrOpcode binop_to_icmp_opcode(BinOpKind op)
{
    switch (op)
    {
        case BIN_EQ:
            return OP_ICMP_EQ;
        case BIN_NE:
            return OP_ICMP_NE;
        case BIN_LT:
            return OP_ICMP_SLT;
        case BIN_GT:
            return OP_ICMP_SGT;
        case BIN_LE:
            return OP_ICMP_SLE;
        case BIN_GE:
            return OP_ICMP_SGE;
        default:
            return OP_ICMP_EQ;
    }
}

static void emit_arith_binop(Block *bb, u32 dst, BinOpKind op, Operand left, Operand right,
                             ASTNode *node)
{
    switch (op)
    {
        case BIN_ADD:
            ir_emit_add(bb, dst, left, right);
            break;
        case BIN_SUB:
            ir_emit_sub(bb, dst, left, right);
            break;
        case BIN_MUL:
            ir_emit_mul(bb, dst, left, right);
            break;
        case BIN_DIV:
            ir_emit_sdiv(bb, dst, left, right);
            break;
        case BIN_REM:
            ir_emit_srem(bb, dst, left, right);
            break;
        case BIN_AND:
            ir_emit_and(bb, dst, left, right);
            break;
        case BIN_OR:
            ir_emit_or(bb, dst, left, right);
            break;
        case BIN_XOR:
            ir_emit_xor(bb, dst, left, right);
            break;
        case BIN_SHL:
            ir_emit_shl(bb, dst, left, right);
            break;
        case BIN_SHR:
            ir_emit_ashr(bb, dst, left, right);
            break;
        case BIN_EQ:
        case BIN_NE:
        case BIN_LT:
        case BIN_GT:
        case BIN_LE:
        case BIN_GE:
            ir_error(node, "comparison is handled separately");
            break;
        case BIN_ASSIGN:
            ir_error(node, "assignment is handled before arithmetic emission");
            break;
        case BIN_LOG_AND:
        case BIN_LOG_OR:
            ir_error(node, "logical operators are handled separately");
            break;
        default:
            ir_error(node, "unsupported binary operator");
            break;
    }
}

static void emit_unary_op(Block *bb, u32 dst, UnaryOpKind op, Operand src, ASTNode *node)
{
    switch (op)
    {
        case UN_NEG:
            ir_emit_neg(bb, dst, src);
            break;
        case UN_BIT_NOT:
            ir_emit_not(bb, dst, src);
            break;
        case UN_LOG_NOT:
            ir_error(node, "logical not is handled separately");
            break;
        default:
            ir_error(node, "unsupported unary operator");
            break;
    }
}

static Operand build_assignment(Block *bb, ASTNode *node, Operand value, BuilderCtx *ctx)
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

static ExprResult build_logical_and(ASTNode *left_node, ASTNode *right_node, Function *f, Block *bb,
                                    BuilderCtx *ctx, Loc loc)
{
    ExprResult left = build_expr(left_node, f, bb, ctx);
    if (!left.bb)
    {
        return left;
    }

    Block *right_bb = add_unique_block(f, "land_rhs");
    Block *true_bb = add_unique_block(f, "land_true");
    Block *false_bb = add_unique_block(f, "land_false");
    Block *merge_bb = add_unique_block(f, "land_merge");

    ir_emit_brcond(left.bb, left.val, right_bb->label, false_bb->label);
    vec_push(right_bb->preds, left.bb);
    vec_push(false_bb->preds, left.bb);

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
    Instr *phi = ir_emit_phi(merge_bb, dst, 2);
    ir_phi_add_entry(phi, ir_operand_imm(1), true_bb);
    ir_phi_add_entry(phi, ir_operand_imm(0), false_bb);
    Instr *last = (Instr *) vec_pop(merge_bb->instrs);
    ASSERT(last == phi);
    vec_insert(merge_bb->instrs, 0, phi);

    (void) loc;
    return expr_result(ir_operand_vreg(dst), merge_bb);
}

static ExprResult build_logical_or(ASTNode *left_node, ASTNode *right_node, Function *f, Block *bb,
                                   BuilderCtx *ctx, Loc loc)
{
    ExprResult left = build_expr(left_node, f, bb, ctx);
    if (!left.bb)
    {
        return left;
    }

    Block *right_bb = add_unique_block(f, "lor_rhs");
    Block *true_bb = add_unique_block(f, "lor_true");
    Block *false_bb = add_unique_block(f, "lor_false");
    Block *merge_bb = add_unique_block(f, "lor_merge");

    ir_emit_brcond(left.bb, left.val, true_bb->label, right_bb->label);
    vec_push(true_bb->preds, left.bb);
    vec_push(right_bb->preds, left.bb);

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

    seal_block(ctx, left.bb);
    seal_block(ctx, right_bb);
    seal_block(ctx, false_bb);
    seal_block(ctx, true_bb);

    merge_bb->sealed = true;

    u32 dst = ir_alloc_vreg(ctx->mod, 4);
    Instr *phi = ir_emit_phi(merge_bb, dst, 2);
    ir_phi_add_entry(phi, ir_operand_imm(1), true_bb);
    ir_phi_add_entry(phi, ir_operand_imm(0), false_bb);
    Instr *last = (Instr *) vec_pop(merge_bb->instrs);
    ASSERT(last == phi);
    vec_insert(merge_bb->instrs, 0, phi);

    (void) loc;
    return expr_result(ir_operand_vreg(dst), merge_bb);
}

static ExprResult build_ternary(ASTNode *cond_node, ASTNode *then_node, ASTNode *else_node,
                                Function *f, Block *bb, BuilderCtx *ctx, Loc loc)
{
    ExprResult cond = build_expr(cond_node, f, bb, ctx);
    if (!cond.bb)
    {
        return cond;
    }

    Block *then_bb = add_unique_block(f, "tern_then");
    Block *else_bb = add_unique_block(f, "tern_else");
    Block *merge_bb = add_unique_block(f, "tern_merge");

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
    Instr *phi = ir_emit_phi(merge_bb, dst, 2);
    ir_phi_add_entry(phi, then_val.val, then_val.bb);
    ir_phi_add_entry(phi, else_val.val, else_val.bb);
    merge_bb->sealed = true;

    (void) loc;
    return expr_result(ir_operand_vreg(dst), merge_bb);
}

static ExprResult build_expr(ASTNode *node, Function *f, Block *bb, BuilderCtx *ctx)
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
                return build_logical_and(be->left, be->right, f, bb, ctx, be->base.loc);
            }
            if (be->op == BIN_LOG_OR)
            {
                return build_logical_or(be->left, be->right, f, bb, ctx, be->base.loc);
            }
            ExprResult left = build_expr(be->left, f, bb, ctx);
            bb = left.bb;
            if (be->op == BIN_ASSIGN)
            {
                ExprResult right = build_expr(be->right, f, bb, ctx);
                bb = right.bb;
                Operand val = build_assignment(bb, node, right.val, ctx);
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
                {
                    ir_emit_icmp(bb, binop_to_icmp_opcode(be->op), dst, left.val, right.val);
                    break;
                }
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
                ir_emit_icmp(bb, OP_ICMP_EQ, dst, src.val, ir_operand_imm(0));
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
            Operand *args = arena_alloc(ctx->mod->arena, nargs * sizeof(Operand), sizeof(Operand));
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
            return build_ternary(te->cond, te->then_expr, te->else_expr, f, bb, ctx, te->base.loc);
        }
        default:
            ir_error(node, "unsupported expression kind %s", ast_kind_name(node->kind));
            return expr_result(ir_operand_imm(0), bb);
    }
}

static Block *build_stmt(ASTNode *node, Function *f, Block *bb, BuilderCtx *ctx);

static void collect_labels(ASTNode *node, Function *f, BuilderCtx *ctx)
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
                Block *label_bb = ir_func_add_block(f, ls->label);
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

static Block *build_stmt_sequence(Vec *stmts, Function *f, Block *bb, BuilderCtx *ctx)
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
            Block *unreach = add_unique_block(f, "unreach");
            bb = build_stmt(stmt, f, unreach, ctx);
        }
        else
        {
            bb = build_stmt(stmt, f, bb, ctx);
        }
    }
    return bb;
}

static Block *build_compound_stmt(ASTCompoundStmt *cs, Function *f, Block *bb, BuilderCtx *ctx)
{
    return build_stmt_sequence(cs->stmts, f, bb, ctx);
}

/* Build one if/else branch: wire the entry, build the statement (if any), and
   fall through to the merge block unless the branch already terminates. */
static void build_branch(ASTNode *branch_stmt, Function *f, Block *cond_bb, Block *branch_bb,
                         Block *merge_bb, BuilderCtx *ctx)
{
    vec_push(branch_bb->preds, cond_bb);
    seal_block(ctx, branch_bb);

    Block *end = branch_bb;
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

static Block *build_if_stmt(ASTIfStmt *is, Function *f, Block *bb, BuilderCtx *ctx)
{
    ExprResult cond = build_expr(is->cond, f, bb, ctx);
    bb = cond.bb;

    Block *then_bb = add_unique_block(f, "then");
    Block *else_bb = add_unique_block(f, "else");
    Block *merge_bb = add_unique_block(f, "merge");

    ir_emit_brcond(bb, cond.val, then_bb->label, else_bb->label);

    build_branch(is->then_branch, f, bb, then_bb, merge_bb, ctx);
    build_branch(is->else_branch, f, bb, else_bb, merge_bb, ctx);
    seal_block(ctx, merge_bb);
    return merge_bb;
}

static Block *build_while_stmt(ASTWhileStmt *ws, Function *f, Block *bb, BuilderCtx *ctx)
{
    Block *header_bb = add_unique_block(f, "while_header");
    Block *body_bb = add_unique_block(f, "while_body");
    Block *exit_bb = add_unique_block(f, "while_exit");
    header_bb->is_loop_header = true;

    ir_emit_br(bb, header_bb->label);
    vec_push(header_bb->preds, bb);
    vec_push(body_bb->preds, header_bb);

    LoopContext loop = {header_bb, exit_bb};
    vec_push(ctx->loop_stack, &loop);
    Block *body_end = build_stmt(ws->body, f, body_bb, ctx);
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

static Block *build_do_while_stmt(ASTDoWhileStmt *ds, Function *f, Block *bb, BuilderCtx *ctx)
{
    Block *body_bb = add_unique_block(f, "do_body");
    Block *header_bb = add_unique_block(f, "do_header");
    Block *exit_bb = add_unique_block(f, "do_exit");
    header_bb->is_loop_header = true;

    ir_emit_br(bb, body_bb->label);
    vec_push(body_bb->preds, bb);
    vec_push(body_bb->preds, header_bb);

    LoopContext loop = {header_bb, exit_bb};
    vec_push(ctx->loop_stack, &loop);
    Block *body_end = build_stmt(ds->body, f, body_bb, ctx);
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

static Block *build_for_stmt(ASTForStmt *fs, Function *f, Block *bb, BuilderCtx *ctx)
{
    if (fs->init)
    {
        bb = build_stmt(fs->init, f, bb, ctx);
    }

    Block *header_bb = add_unique_block(f, "for_header");
    Block *body_bb = add_unique_block(f, "for_body");
    Block *latch_bb = add_unique_block(f, "for_latch");
    Block *exit_bb = add_unique_block(f, "for_exit");
    header_bb->is_loop_header = true;

    ir_emit_br(bb, header_bb->label);
    vec_push(header_bb->preds, bb);
    vec_push(body_bb->preds, header_bb);

    LoopContext loop = {latch_bb, exit_bb};
    vec_push(ctx->loop_stack, &loop);
    Block *body_end = build_stmt(fs->body, f, body_bb, ctx);
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

static Block *build_break_stmt(ASTBreakStmt *bs, Function *f, Block *bb, BuilderCtx *ctx)
{
    (void) bs;
    (void) f;
    ASSERT(vec_size(ctx->loop_stack) > 0);
    LoopContext *loop = (LoopContext *) vec_last(ctx->loop_stack);
    ir_emit_br(bb, loop->exit->label);
    vec_push(loop->exit->preds, bb);
    return bb;
}

static Block *build_continue_stmt(ASTContinueStmt *cs, Function *f, Block *bb, BuilderCtx *ctx)
{
    (void) cs;
    (void) f;
    ASSERT(vec_size(ctx->loop_stack) > 0);
    LoopContext *loop = (LoopContext *) vec_last(ctx->loop_stack);
    ir_emit_br(bb, loop->header->label);
    vec_push(loop->header->preds, bb);
    return bb;
}

static Block *build_goto_stmt(ASTGotoStmt *gs, Function *f, Block *bb, BuilderCtx *ctx)
{
    (void) f;
    Block *target = (Block *) strmap_get(ctx->goto_labels, gs->label);
    ASSERT(target != NULL);
    ir_emit_br(bb, target->label);
    vec_push(target->preds, bb);
    return bb;
}

static Block *build_label_stmt(ASTLabelStmt *ls, Function *f, Block *bb, BuilderCtx *ctx)
{
    Block *label_bb = (Block *) strmap_get(ctx->goto_labels, ls->label);
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

static Block *build_stmt(ASTNode *node, Function *f, Block *bb, BuilderCtx *ctx)
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
            Operand val = ir_operand_imm(0);
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

static bool build_func(ASTNode *ast, Module *mod)
{
    if (ast->kind != AST_FUNC_DEF)
    {
        ir_error(ast, "expected function definition at top level");
        return false;
    }
    ASTFuncDef *func_ast = (ASTFuncDef *) ast;

    Function *func = ir_module_add_func(mod, func_ast->name, func_ast->ret_type);
    Block *entry = ir_func_add_block(func, "entry");

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
        Param *p = arena_alloc(mod->arena, sizeof(Param), sizeof(void *));
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
        Block *bb = (Block *) vec_get(func->blocks, i);
        seal_block(&ctx, bb);
    }

    return true;
}

Module *ir_build_module(ASTNode *ast, Arena *arena)
{
    if (ast->kind != AST_PROGRAM)
    {
        ir_error(ast, "expected program at top level");
        return NULL;
    }
    ASTProgram *prog = (ASTProgram *) ast;

    Module *mod = ir_module_new(arena);
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
