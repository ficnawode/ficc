#include "semantic.h"
#include "util/hashmap.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct SemanticCtx SemanticCtx;
struct SemanticCtx
{
    Arena *arena;
    StrMap *globals; /* function name -> ASTFuncDef */
    StrMap *locals;  /* variable name -> ASTVarDecl */
    bool error;
};

static void sem_error(Loc loc, const char *fmt, ...)
{
    fprintf(stderr, "%s:%u:%u: [semantic] error: ", loc.file, loc.line, loc.col);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

static bool check_expr(ASTNode *node, SemanticCtx *ctx)
{
    switch (node->kind)
    {
        case AST_INT_LITERAL:
            return true;
        case AST_IDENT: {
            ASTIdent *id = ast_as(ASTIdent, node);
            if (!strmap_get(ctx->locals, id->name))
            {
                sem_error(node->loc, "undeclared identifier '%s'", id->name);
                ctx->error = true;
                return false;
            }
            return true;
        }
        case AST_BINARY_EXPR: {
            ASTBinaryExpr *be = ast_as(ASTBinaryExpr, node);
            return check_expr(be->left, ctx) && check_expr(be->right, ctx);
        }
        case AST_UNARY_EXPR: {
            ASTUnaryExpr *ue = ast_as(ASTUnaryExpr, node);
            return check_expr(ue->operand, ctx);
        }
        case AST_CALL_EXPR: {
            ASTCallExpr *ce = ast_as(ASTCallExpr, node);
            ASTFuncDef *callee = strmap_get(ctx->globals, ce->callee);
            if (!callee)
            {
                sem_error(node->loc, "undeclared function '%s'", ce->callee);
                ctx->error = true;
                return false;
            }
            size_t expected = vec_size(callee->params);
            size_t got = vec_size(ce->args);
            if (expected != got)
            {
                sem_error(node->loc, "function '%s' expects %zu arguments, got %zu", ce->callee,
                          expected, got);
                ctx->error = true;
                return false;
            }
            size_t n = vec_size(ce->args);
            for (size_t i = 0; i < n; i++)
            {
                if (!check_expr((ASTNode *)vec_get(ce->args, i), ctx))
                    return false;
            }
            return true;
        }
        default:
            sem_error(node->loc, "unsupported expression kind %s", ast_kind_name(node->kind));
            ctx->error = true;
            return false;
    }
}

static bool check_stmt(ASTNode *node, SemanticCtx *ctx, Type *ret_type)
{
    switch (node->kind)
    {
        case AST_RETURN_STMT: {
            ASTReturnStmt *ret = ast_as(ASTReturnStmt, node);
            if (ret_type->kind == TYPE_VOID)
            {
                if (ret->expr)
                {
                    sem_error(node->loc, "void function should not return a value");
                    ctx->error = true;
                    return false;
                }
            }
            else
            {
                if (!ret->expr)
                {
                    sem_error(node->loc, "non-void function must return a value");
                    ctx->error = true;
                    return false;
                }
            }
            if (ret->expr && !check_expr(ret->expr, ctx))
                return false;
            return true;
        }
        case AST_VAR_DECL: {
            ASTVarDecl *vd = ast_as(ASTVarDecl, node);
            if (strmap_get(ctx->locals, vd->name))
            {
                sem_error(node->loc, "redeclaration of '%s'", vd->name);
                ctx->error = true;
                return false;
            }
            strmap_set(ctx->locals, vd->name, vd);
            if (vd->init && !check_expr(vd->init, ctx))
                return false;
            return true;
        }
        case AST_EXPR_STMT: {
            ASTExprStmt *es = ast_as(ASTExprStmt, node);
            return check_expr(es->expr, ctx);
        }
        default:
            sem_error(node->loc, "unsupported statement kind %s", ast_kind_name(node->kind));
            ctx->error = true;
            return false;
    }
}

static bool check_func(ASTNode *node, SemanticCtx *ctx)
{
    ASSERT(node->kind == AST_FUNC_DEF);
    ASTFuncDef *fn = ast_as(ASTFuncDef, node);

    /* Set up local scope: parameters + locals */
    StrMap *saved_locals = ctx->locals;
    ctx->locals = strmap_new(ctx->arena);

    /* Insert parameters into local scope */
    size_t nparams = vec_size(fn->params);
    for (size_t i = 0; i < nparams; i++)
    {
        ASTVarDecl *param = ast_as(ASTVarDecl, (ASTNode *)vec_get(fn->params, i));
        if (strmap_get(ctx->locals, param->name))
        {
            sem_error(param->base.loc, "redeclaration of parameter '%s'", param->name);
            ctx->error = true;
            ctx->locals = saved_locals;
            return false;
        }
        strmap_set(ctx->locals, param->name, param);
    }

    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    size_t nstmts = vec_size(body->stmts);
    for (size_t i = 0; i < nstmts; i++)
    {
        ASTNode *stmt = (ASTNode *)vec_get(body->stmts, i);
        if (!check_stmt(stmt, ctx, fn->ret_type))
        {
            ctx->locals = saved_locals;
            return false;
        }
    }

    ctx->locals = saved_locals;
    return true;
}

ASTNode *semantic_check(ASTNode *ast, Arena *arena)
{
    if (!ast)
        return NULL;
    if (ast->kind != AST_PROGRAM)
    {
        sem_error(ast->loc, "expected program at top level");
        return NULL;
    }

    ASTProgram *prog = ast_as(ASTProgram, ast);
    SemanticCtx ctx = {arena, strmap_new(arena), NULL, false};

    /* Pass 1: collect all function definitions */
    size_t ndecls = vec_size(prog->decls);
    for (size_t i = 0; i < ndecls; i++)
    {
        ASTNode *decl = (ASTNode *)vec_get(prog->decls, i);
        if (decl->kind != AST_FUNC_DEF)
        {
            sem_error(decl->loc, "expected function definition at top level");
            return NULL;
        }
        ASTFuncDef *fn = ast_as(ASTFuncDef, decl);
        if (strmap_get(ctx.globals, fn->name))
        {
            sem_error(decl->loc, "redefinition of function '%s'", fn->name);
            return NULL;
        }
        strmap_set(ctx.globals, fn->name, fn);
    }

    /* Pass 2: check each function body */
    for (size_t i = 0; i < ndecls; i++)
    {
        ASTNode *decl = (ASTNode *)vec_get(prog->decls, i);
        if (!check_func(decl, &ctx))
            return NULL;
    }

    return ast;
}
