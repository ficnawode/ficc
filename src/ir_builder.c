#include "ir_builder.h"
#include "util/hashmap.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct BlockState BlockState;
struct BlockState
{
    StrMap *locals; /* name -> Operand* */
};

typedef struct BuilderCtx BuilderCtx;
struct BuilderCtx
{
    Arena *arena;
    Module *mod;
    U64Map *block_states; /* (u64)Block* -> BlockState* */
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

static Operand *make_operand_ptr(Arena *arena, Operand op)
{
    Operand *p = arena_alloc(arena, sizeof(Operand), sizeof(Operand));
    *p = op;
    return p;
}

static BlockState *get_block_state(BuilderCtx *ctx, Block *bb)
{
    BlockState *bs = u64map_get(ctx->block_states, (u64) (uintptr_t) bb);
    if (!bs)
    {
        bs = arena_alloc(ctx->arena, sizeof(BlockState), sizeof(void *));
        bs->locals = strmap_new(ctx->arena);
        u64map_set(ctx->block_states, (u64) (uintptr_t) bb, bs);
    }
    return bs;
}

static bool is_terminated(Block *bb)
{
    size_t n = vec_size(bb->instrs);
    if (n == 0)
        return false;
    Instr *last = (Instr *) vec_get(bb->instrs, n - 1);
    return last->opcode == OP_RET || last->opcode == OP_UNREACHABLE || last->opcode == OP_BR ||
           last->opcode == OP_BRCOND;
}

static Operand read_variable(BuilderCtx *ctx, const char *name, Block *bb);

static Operand read_variable_recursive(BuilderCtx *ctx, const char *name, Block *bb)
{
    BlockState *bs = get_block_state(ctx, bb);
    Operand val;
    if (vec_size(bb->preds) == 0)
    {
        /* No predecessors: undefined (semantic already checked) */
        val = ir_operand_imm(0);
    }
    else if (vec_size(bb->preds) == 1)
    {
        Block *pred = (Block *) vec_get(bb->preds, 0);
        val = read_variable(ctx, name, pred);
    }
    else
    {
        /* Multiple predecessors: need a PHI */
        u32 nentries = (u32) vec_size(bb->preds);
        u32 dst = ir_alloc_vreg(ctx->mod, 4);
        Instr *phi = ir_emit_phi(bb, ctx->arena, dst, nentries);
        for (u32 e = 0; e < nentries; e++)
        {
            Block *pred = (Block *) vec_get(bb->preds, e);
            Operand pval = read_variable(ctx, name, pred);
            phi_add_entry(phi, pval, pred);
        }
        val = ir_operand_vreg(dst);
    }
    strmap_set(bs->locals, name, make_operand_ptr(ctx->arena, val));
    return val;
}

static Operand read_variable(BuilderCtx *ctx, const char *name, Block *bb)
{
    BlockState *bs = get_block_state(ctx, bb);
    Operand *p = strmap_get(bs->locals, name);
    if (p)
        return *p;
    return read_variable_recursive(ctx, name, bb);
}

static void write_variable(BuilderCtx *ctx, const char *name, Block *bb, Operand val)
{
    BlockState *bs = get_block_state(ctx, bb);
    strmap_set(bs->locals, name, make_operand_ptr(ctx->arena, val));
}

static Operand build_expr(ASTNode *node, Function *f, Block *bb, BuilderCtx *ctx)
{
    (void) f;
    switch (node->kind)
    {
        case AST_INT_LITERAL:
        {
            ASTIntLiteral *lit = (ASTIntLiteral *) node;
            return ir_operand_imm(lit->value);
        }
        case AST_IDENT:
        {
            ASTIdent *id = ast_as(ASTIdent, node);
            return read_variable(ctx, id->name, bb);
        }
        case AST_BINARY_EXPR:
        {
            ASTBinaryExpr *be = ast_as(ASTBinaryExpr, node);
            Operand left = build_expr(be->left, f, bb, ctx);
            Operand right = build_expr(be->right, f, bb, ctx);
            if (be->op == BIN_ASSIGN)
            {
                if (be->left->kind != AST_IDENT)
                {
                    ir_error(node, "assignment target must be an identifier");
                    return ir_operand_imm(0);
                }
                ASTIdent *target = ast_as(ASTIdent, be->left);
                write_variable(ctx, target->name, bb, right);
                return right;
            }
            u32 dst = ir_alloc_vreg(ctx->mod, 4);
            switch (be->op)
            {
                case BIN_ADD:
                    ir_emit_add(bb, ctx->arena, dst, left, right);
                    break;
                case BIN_SUB:
                    ir_emit_sub(bb, ctx->arena, dst, left, right);
                    break;
                case BIN_MUL:
                    ir_emit_mul(bb, ctx->arena, dst, left, right);
                    break;
                case BIN_DIV:
                    ir_emit_sdiv(bb, ctx->arena, dst, left, right);
                    break;
                case BIN_REM:
                    ir_emit_srem(bb, ctx->arena, dst, left, right);
                    break;
                default:
                    ir_error(node, "unsupported binary operator");
                    break;
            }
            return ir_operand_vreg(dst);
        }
        case AST_UNARY_EXPR:
        {
            ASTUnaryExpr *ue = ast_as(ASTUnaryExpr, node);
            Operand src = build_expr(ue->operand, f, bb, ctx);
            u32 dst = ir_alloc_vreg(ctx->mod, 4);
            switch (ue->op)
            {
                case UN_NEG:
                    ir_emit_neg(bb, ctx->arena, dst, src);
                    break;
                default:
                    ir_error(node, "unsupported unary operator");
                    break;
            }
            return ir_operand_vreg(dst);
        }
        case AST_CALL_EXPR:
        {
            ASTCallExpr *ce = ast_as(ASTCallExpr, node);
            u32 nargs = (u32) vec_size(ce->args);
            Operand *args = arena_alloc(ctx->arena, nargs * sizeof(Operand), sizeof(Operand));
            for (u32 i = 0; i < nargs; i++)
            {
                ASTNode *arg = (ASTNode *) vec_get(ce->args, i);
                args[i] = build_expr(arg, f, bb, ctx);
            }
            u32 dst = ir_alloc_vreg(ctx->mod, 4);
            ir_emit_call(bb, ctx->arena, dst, ce->callee, nargs, args);
            return ir_operand_vreg(dst);
        }
        default:
            ir_error(node, "unsupported expression kind %s", ast_kind_name(node->kind));
            return ir_operand_imm(0);
    }
}

static Block *build_stmt(ASTNode *node, Function *f, Block *bb, BuilderCtx *ctx);

static Block *build_compound_stmt(ASTCompoundStmt *cs, Function *f, Block *bb, BuilderCtx *ctx)
{
    size_t n = vec_size(cs->stmts);
    for (size_t i = 0; i < n; i++)
    {
        ASTNode *stmt = (ASTNode *) vec_get(cs->stmts, i);
        bb = build_stmt(stmt, f, bb, ctx);
    }
    return bb;
}

static Block *build_stmt(ASTNode *node, Function *f, Block *bb, BuilderCtx *ctx)
{
    switch (node->kind)
    {
        case AST_RETURN_STMT:
        {
            ASTReturnStmt *ret = (ASTReturnStmt *) node;
            if (ret->expr)
            {
                Operand val = build_expr(ret->expr, f, bb, ctx);
                ir_emit_ret(bb, ctx->arena, val);
            }
            else
            {
                ir_emit_ret_void(bb, ctx->arena);
            }
            return bb;
        }
        case AST_VAR_DECL:
        {
            ASTVarDecl *vd = ast_as(ASTVarDecl, node);
            Operand val = ir_operand_imm(0);
            if (vd->init)
            {
                val = build_expr(vd->init, f, bb, ctx);
            }
            write_variable(ctx, vd->name, bb, val);
            return bb;
        }
        case AST_EXPR_STMT:
        {
            ASTExprStmt *es = ast_as(ASTExprStmt, node);
            build_expr(es->expr, f, bb, ctx);
            return bb;
        }
        case AST_COMPOUND_STMT:
        {
            return build_compound_stmt(ast_as(ASTCompoundStmt, node), f, bb, ctx);
        }
        case AST_IF_STMT:
        {
            ASTIfStmt *is = ast_as(ASTIfStmt, node);
            Operand cond = build_expr(is->cond, f, bb, ctx);

            Block *then_bb = ir_func_add_block(f, ctx->arena, "then");
            Block *else_bb = ir_func_add_block(f, ctx->arena, "else");
            Block *merge_bb = ir_func_add_block(f, ctx->arena, "merge");

            ir_emit_brcond(bb, ctx->arena, cond, then_bb->label, else_bb->label);

            /* Then branch */
            vec_push(then_bb->preds, bb);
            then_bb->sealed = true;
            Block *then_end = build_stmt(is->then_branch, f, then_bb, ctx);
            bool then_reaches_merge = !is_terminated(then_end);
            if (then_reaches_merge)
                ir_emit_br(then_end, ctx->arena, merge_bb->label);

            /* Else branch */
            vec_push(else_bb->preds, bb);
            else_bb->sealed = true;
            Block *else_end = else_bb;
            if (is->else_branch)
            {
                else_end = build_stmt(is->else_branch, f, else_bb, ctx);
            }
            bool else_reaches_merge = !is_terminated(else_end);
            if (else_reaches_merge)
                ir_emit_br(else_end, ctx->arena, merge_bb->label);

            /* Merge */
            if (then_reaches_merge)
                vec_push(merge_bb->preds, then_end);
            if (else_reaches_merge)
                vec_push(merge_bb->preds, else_end);
            merge_bb->sealed = true;
            return merge_bb;
        }
        default:
            ir_error(node, "unsupported statement kind %s", ast_kind_name(node->kind));
            return bb;
    }
}

static bool build_func(ASTNode *ast, Module *mod, Arena *arena)
{
    if (ast->kind != AST_FUNC_DEF)
    {
        ir_error(ast, "expected function definition at top level");
        return false;
    }
    ASTFuncDef *func_ast = (ASTFuncDef *) ast;

    Function *func = ir_module_add_func(mod, arena, func_ast->name, func_ast->ret_type);
    Block *entry = ir_func_add_block(func, arena, "entry");

    if (func_ast->body->kind != AST_COMPOUND_STMT)
    {
        ir_error(func_ast->body, "expected compound statement as function body");
        return false;
    }

    BuilderCtx ctx = {arena, mod, u64map_new(arena)};

    /* Allocate vregs for parameters and record them */
    size_t nparams = vec_size(func_ast->params);
    for (size_t i = 0; i < nparams; i++)
    {
        ASTVarDecl *param = ast_as(ASTVarDecl, (ASTNode *) vec_get(func_ast->params, i));
        u32 vreg = ir_alloc_vreg(mod, 4);
        Param *p = arena_alloc(arena, sizeof(Param), sizeof(void *));
        p->name = param->name;
        p->type = param->type;
        p->vreg = vreg;
        vec_push(func->params, p);
        write_variable(&ctx, param->name, entry, ir_operand_vreg(vreg));
    }

    ASTCompoundStmt *body = (ASTCompoundStmt *) func_ast->body;
    size_t nstmts = vec_size(body->stmts);
    for (size_t i = 0; i < nstmts; i++)
    {
        ASTNode *stmt = (ASTNode *) vec_get(body->stmts, i);
        entry = build_stmt(stmt, func, entry, &ctx);
    }

    /* Ensure block ends with a terminator */
    if (!is_terminated(entry))
    {
        if (func_ast->ret_type->kind == TYPE_VOID)
            ir_emit_ret_void(entry, arena);
        else
            ir_emit_unreachable(entry, arena);
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
        if (!build_func(decl, mod, arena))
            return NULL;
    }
    return mod;
}
