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

static bool check_expr(ASTNode *node, SemanticCtx *ctx);
static bool check_stmt(ASTNode *node, SemanticCtx *ctx, Type *ret_type);
static bool check_func(ASTNode *node, SemanticCtx *ctx);

static void sem_error(Loc loc, const char *fmt, ...)
{
    fprintf(stderr, "%s:%u:%u: [semantic] error: ", loc.file, loc.line, loc.col);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

static bool check_identifier_expr(ASTIdent *ident, SemanticCtx *ctx)
{
    if (!strmap_get(ctx->locals, ident->name))
    {
        sem_error(ident->base.loc, "undeclared identifier '%s'", ident->name);
        ctx->error = true;
        return false;
    }
    return true;
}

static bool check_binary_expr(ASTBinaryExpr *binary_expr, SemanticCtx *ctx)
{
    return check_expr(binary_expr->left, ctx) && check_expr(binary_expr->right, ctx);
}

static bool check_unary_expr(ASTUnaryExpr *unary_expr, SemanticCtx *ctx)
{
    return check_expr(unary_expr->operand, ctx);
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
        if (!check_expr((ASTNode *) vec_get(call_expr->args, i), ctx))
        {
            return false;
        }
    }
    return true;
}

static bool check_expr(ASTNode *node, SemanticCtx *ctx)
{
    switch (node->kind)
    {
        case AST_INT_LITERAL:
            return true;
        case AST_IDENT:
            return check_identifier_expr(ast_as(ASTIdent, node), ctx);
        case AST_BINARY_EXPR:
            return check_binary_expr(ast_as(ASTBinaryExpr, node), ctx);
        case AST_UNARY_EXPR:
            return check_unary_expr(ast_as(ASTUnaryExpr, node), ctx);
        case AST_CALL_EXPR:
            return check_call_expr(ast_as(ASTCallExpr, node), ctx);
        default:
            sem_error(node->loc, "unsupported expression kind %s", ast_kind_name(node->kind));
            ctx->error = true;
            return false;
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

    return true;
}

static bool check_variable_declaration(ASTVarDecl *var_decl, SemanticCtx *ctx)
{
    if (strmap_get(ctx->locals, var_decl->name))
    {
        sem_error(var_decl->base.loc, "redeclaration of '%s'", var_decl->name);
        ctx->error = true;
        {
            return false;
        }
    }
    strmap_set(ctx->locals, var_decl->name, var_decl);
    if (var_decl->init && !check_expr(var_decl->init, ctx))
    {
        return false;
    }
    return true;
}

static bool check_expression_statement(ASTExprStmt *expr_stmt, SemanticCtx *ctx)
{
    return check_expr(expr_stmt->expr, ctx);
}

static bool check_compound_statement(ASTCompoundStmt *compound_stmt, SemanticCtx *ctx,
                                     Type *ret_type)
{
    size_t nstmts = vec_size(compound_stmt->stmts);
    for (size_t i = 0; i < nstmts; i++)
    {
        ASTNode *stmt = (ASTNode *) vec_get(compound_stmt->stmts, i);
        if (!check_stmt(stmt, ctx, ret_type))
        {
            return false;
        }
    }
    return true;
}

static bool check_if_statement(ASTIfStmt *if_stmt, SemanticCtx *ctx, Type *ret_type)
{
    if (!check_expr(if_stmt->cond, ctx))
        return false;
    if (!check_stmt(if_stmt->then_branch, ctx, ret_type))
        return false;
    if (if_stmt->else_branch && !check_stmt(if_stmt->else_branch, ctx, ret_type))
        return false;
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
        case AST_EXPR_STMT:
            return check_expression_statement(ast_as(ASTExprStmt, node), ctx);
        case AST_COMPOUND_STMT:
            return check_compound_statement(ast_as(ASTCompoundStmt, node), ctx, ret_type);
        case AST_IF_STMT:
            return check_if_statement(ast_as(ASTIfStmt, node), ctx, ret_type);
        default:
            sem_error(node->loc, "unsupported statement kind %s", ast_kind_name(node->kind));
            ctx->error = true;
            return false;
    }
}

static bool setup_function_locals(ASTFuncDef *func_def, SemanticCtx *ctx)
{
    /* Set up local scope: parameters + locals */
    StrMap *saved_locals = ctx->locals;
    ctx->locals = strmap_new(ctx->arena);

    /* Insert parameters into local scope */
    size_t nparams = vec_size(func_def->params);
    for (size_t i = 0; i < nparams; i++)
    {
        ASTVarDecl *param = ast_as(ASTVarDecl, (ASTNode *) vec_get(func_def->params, i));
        if (strmap_get(ctx->locals, param->name))
        {
            sem_error(param->base.loc, "redeclaration of parameter '%s'", param->name);
            ctx->error = true;
            ctx->locals = saved_locals;
            return false;
        }
        strmap_set(ctx->locals, param->name, param);
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
            return false;
    }
    return true;
}

static bool check_func(ASTNode *node, SemanticCtx *ctx)
{
    ASSERT(node->kind == AST_FUNC_DEF);
    ASTFuncDef *fn = ast_as(ASTFuncDef, node);

    if (!setup_function_locals(fn, ctx))
        return false;

    bool result = check_function_body(fn, ctx);

    /* Restore outer scope */
    return result;
}

/* Pass 1: Collect all function definitions */
static bool collect_function_definitions(ASTProgram *prog, SemanticCtx *ctx)
{
    size_t ndecls = vec_size(prog->decls);
    for (size_t i = 0; i < ndecls; i++)
    {
        ASTNode *decl = (ASTNode *) vec_get(prog->decls, i);
        if (decl->kind != AST_FUNC_DEF)
        {
            sem_error(decl->loc, "expected function definition at top level");
            return false;
        }
        ASTFuncDef *fn = ast_as(ASTFuncDef, decl);
        if (strmap_get(ctx->globals, fn->name))
        {
            sem_error(decl->loc, "redefinition of function '%s'", fn->name);
            return false;
        }
        strmap_set(ctx->globals, fn->name, fn);
    }
    return true;
}

/* Pass 2: Check each function body */
static bool check_function_bodies(ASTProgram *prog, SemanticCtx *ctx)
{
    size_t ndecls = vec_size(prog->decls);
    for (size_t i = 0; i < ndecls; i++)
    {
        ASTNode *decl = (ASTNode *) vec_get(prog->decls, i);
        if (!check_func(decl, ctx))
            return false;
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
    SemanticCtx ctx = {arena, strmap_new(arena), NULL, false};

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
