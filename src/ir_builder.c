#include "ir_builder.h"
#include "util/hashmap.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct BuilderCtx BuilderCtx;
struct BuilderCtx
{
    Arena *arena;
    Module *mod;
    StrMap *locals; /* name -> Operand* */
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

static Operand *lookup_operand(BuilderCtx *ctx, const char *name)
{
    return strmap_get(ctx->locals, name);
}

static Operand build_expr(ASTNode *node, Function *f, Block *bb, BuilderCtx *ctx)
{
    switch (node->kind) {
    case AST_INT_LITERAL: {
        ASTIntLiteral *lit = (ASTIntLiteral *)node;
        return ir_operand_imm(lit->value);
    }
    case AST_IDENT: {
        ASTIdent *id = ast_as(ASTIdent, node);
        Operand *p = lookup_operand(ctx, id->name);
        if (!p) {
            ir_error(node, "undefined variable '%s'", id->name);
            return ir_operand_imm(0);
        }
        return *p;
    }
    case AST_BINARY_EXPR: {
        ASTBinaryExpr *be = ast_as(ASTBinaryExpr, node);
        Operand left = build_expr(be->left, f, bb, ctx);
        Operand right = build_expr(be->right, f, bb, ctx);
        if (be->op == BIN_ASSIGN) {
            if (be->left->kind != AST_IDENT) {
                ir_error(node, "assignment target must be an identifier");
                return ir_operand_imm(0);
            }
            ASTIdent *target = ast_as(ASTIdent, be->left);
            Operand *p = lookup_operand(ctx, target->name);
            if (!p) {
                ir_error(node, "undefined variable '%s'", target->name);
                return ir_operand_imm(0);
            }
            *p = right;
            return right;
        }
        u32 dst = ir_alloc_vreg(ctx->mod, 4);
        switch (be->op) {
            case BIN_ADD: ir_emit_add(bb, ctx->arena, dst, left, right); break;
            case BIN_SUB: ir_emit_sub(bb, ctx->arena, dst, left, right); break;
            case BIN_MUL: ir_emit_mul(bb, ctx->arena, dst, left, right); break;
            case BIN_DIV: ir_emit_sdiv(bb, ctx->arena, dst, left, right); break;
            case BIN_REM: ir_emit_srem(bb, ctx->arena, dst, left, right); break;
            default:
                ir_error(node, "unsupported binary operator");
                break;
        }
        return ir_operand_vreg(dst);
    }
    case AST_UNARY_EXPR: {
        ASTUnaryExpr *ue = ast_as(ASTUnaryExpr, node);
        Operand src = build_expr(ue->operand, f, bb, ctx);
        u32 dst = ir_alloc_vreg(ctx->mod, 4);
        switch (ue->op) {
            case UN_NEG: ir_emit_neg(bb, ctx->arena, dst, src); break;
            default:
                ir_error(node, "unsupported unary operator");
                break;
        }
        return ir_operand_vreg(dst);
    }
    case AST_CALL_EXPR: {
        ASTCallExpr *ce = ast_as(ASTCallExpr, node);
        u32 nargs = (u32)vec_size(ce->args);
        Operand *args = arena_alloc(ctx->arena, nargs * sizeof(Operand), sizeof(Operand));
        for (u32 i = 0; i < nargs; i++) {
            ASTNode *arg = (ASTNode *)vec_get(ce->args, i);
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

static void build_stmt(ASTNode *node, Function *f, Block *bb, BuilderCtx *ctx)
{
    switch (node->kind) {
    case AST_RETURN_STMT: {
        ASTReturnStmt *ret = (ASTReturnStmt *)node;
        if (ret->expr) {
            Operand val = build_expr(ret->expr, f, bb, ctx);
            ir_emit_ret(bb, ctx->arena, val);
        } else {
            ir_emit_ret_void(bb, ctx->arena);
        }
        return;
    }
    case AST_VAR_DECL: {
        ASTVarDecl *vd = ast_as(ASTVarDecl, node);
        Operand val = ir_operand_vreg(ir_alloc_vreg(ctx->mod, 4));
        if (vd->init) {
            val = build_expr(vd->init, f, bb, ctx);
        }
        strmap_set(ctx->locals, vd->name, make_operand_ptr(ctx->arena, val));
        return;
    }
    case AST_EXPR_STMT: {
        ASTExprStmt *es = ast_as(ASTExprStmt, node);
        build_expr(es->expr, f, bb, ctx);
        return;
    }
    default:
        ir_error(node, "unsupported statement kind %s", ast_kind_name(node->kind));
        return;
    }
}

static bool build_func(ASTNode *ast, Module *mod, Arena *arena)
{
    if (ast->kind != AST_FUNC_DEF) {
        ir_error(ast, "expected function definition at top level");
        return false;
    }
    ASTFuncDef *func_ast = (ASTFuncDef *)ast;

    Function *func = ir_module_add_func(mod, arena, func_ast->name, func_ast->ret_type);
    Block *entry = ir_func_add_block(func, arena, "entry");

    if (func_ast->body->kind != AST_COMPOUND_STMT) {
        ir_error(func_ast->body, "expected compound statement as function body");
        return false;
    }

    BuilderCtx ctx = {arena, mod, strmap_new(arena)};

    /* Allocate vregs for parameters and record them */
    size_t nparams = vec_size(func_ast->params);
    for (size_t i = 0; i < nparams; i++) {
        ASTVarDecl *param = ast_as(ASTVarDecl, (ASTNode *)vec_get(func_ast->params, i));
        u32 vreg = ir_alloc_vreg(mod, 4);
        Param *p = arena_alloc(arena, sizeof(Param), sizeof(void *));
        p->name = param->name;
        p->type = param->type;
        p->vreg = vreg;
        vec_push(func->params, p);
        strmap_set(ctx.locals, param->name, make_operand_ptr(arena, ir_operand_vreg(vreg)));
    }

    ASTCompoundStmt *body = (ASTCompoundStmt *)func_ast->body;
    size_t nstmts = vec_size(body->stmts);
    for (size_t i = 0; i < nstmts; i++) {
        ASTNode *stmt = (ASTNode *)vec_get(body->stmts, i);
        build_stmt(stmt, func, entry, &ctx);
    }

    /* Ensure block ends with a terminator */
    if (vec_size(entry->instrs) == 0) {
        if (func_ast->ret_type->kind == TYPE_VOID)
            ir_emit_ret_void(entry, arena);
        else
            ir_emit_unreachable(entry, arena);
    } else {
        Instr *last = (Instr *)vec_get(entry->instrs, vec_size(entry->instrs) - 1);
        if (last->opcode != OP_RET && last->opcode != OP_UNREACHABLE &&
            last->opcode != OP_BR && last->opcode != OP_BRCOND) {
            if (func_ast->ret_type->kind == TYPE_VOID)
                ir_emit_ret_void(entry, arena);
            else
                ir_emit_unreachable(entry, arena);
        }
    }

    return true;
}

Module *ir_build_module(ASTNode *ast, Arena *arena)
{
    if (ast->kind != AST_PROGRAM) {
        ir_error(ast, "expected program at top level");
        return NULL;
    }
    ASTProgram *prog = (ASTProgram *)ast;

    Module *mod = ir_module_new(arena);
    size_t ndecls = vec_size(prog->decls);
    for (size_t i = 0; i < ndecls; i++) {
        ASTNode *decl = (ASTNode *)vec_get(prog->decls, i);
        if (!build_func(decl, mod, arena))
            return NULL;
    }
    return mod;
}
