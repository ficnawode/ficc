#include "ast.h"
#include <stdio.h>

const char *ast_kind_name(ASTKind kind)
{
    switch (kind)
    {
#define CASE(K)                                                                                    \
    case K:                                                                                        \
        return #K;
        AST_KINDS(CASE)
#undef CASE
    }
    return "AST_UNKNOWN";
}

ASTNode *ast_func_def(Arena *arena, Type *ret_type, const char *name, Vec *params, ASTNode *body,
                      Loc loc)
{
    ASTFuncDef *n = arena_alloc(arena, sizeof(ASTFuncDef), sizeof(void *));
    n->base.kind = AST_FUNC_DEF;
    n->base.loc = loc;
    n->ret_type = ret_type;
    n->name = name;
    n->params = params;
    n->body = body;
    return &n->base;
}

ASTNode *ast_compound_stmt(Arena *arena, Vec *stmts, Loc loc)
{
    ASTCompoundStmt *n = arena_alloc(arena, sizeof(ASTCompoundStmt), sizeof(void *));
    n->base.kind = AST_COMPOUND_STMT;
    n->base.loc = loc;
    n->stmts = stmts;
    return &n->base;
}

ASTNode *ast_return_stmt(Arena *arena, ASTNode *expr, Loc loc)
{
    ASTReturnStmt *n = arena_alloc(arena, sizeof(ASTReturnStmt), sizeof(void *));
    n->base.kind = AST_RETURN_STMT;
    n->base.loc = loc;
    n->expr = expr;
    return &n->base;
}

ASTNode *ast_int_literal(Arena *arena, i64 value, Loc loc)
{
    ASTIntLiteral *n = arena_alloc(arena, sizeof(ASTIntLiteral), sizeof(void *));
    n->base.kind = AST_INT_LITERAL;
    n->base.loc = loc;
    n->value = value;
    return &n->base;
}

ASTNode *ast_program(Arena *arena, Vec *decls, Loc loc)
{
    ASTProgram *n = arena_alloc(arena, sizeof(ASTProgram), sizeof(void *));
    n->base.kind = AST_PROGRAM;
    n->base.loc = loc;
    n->decls = decls;
    return &n->base;
}

ASTNode *ast_var_decl(Arena *arena, Type *type, const char *name, ASTNode *init, Loc loc)
{
    ASTVarDecl *n = arena_alloc(arena, sizeof(ASTVarDecl), sizeof(void *));
    n->base.kind = AST_VAR_DECL;
    n->base.loc = loc;
    n->type = type;
    n->name = name;
    n->init = init;
    return &n->base;
}

ASTNode *ast_expr_stmt(Arena *arena, ASTNode *expr, Loc loc)
{
    ASTExprStmt *n = arena_alloc(arena, sizeof(ASTExprStmt), sizeof(void *));
    n->base.kind = AST_EXPR_STMT;
    n->base.loc = loc;
    n->expr = expr;
    return &n->base;
}

ASTNode *ast_binary_expr(Arena *arena, BinOpKind op, ASTNode *left, ASTNode *right, Loc loc)
{
    ASTBinaryExpr *n = arena_alloc(arena, sizeof(ASTBinaryExpr), sizeof(void *));
    n->base.kind = AST_BINARY_EXPR;
    n->base.loc = loc;
    n->op = op;
    n->left = left;
    n->right = right;
    return &n->base;
}

ASTNode *ast_unary_expr(Arena *arena, UnaryOpKind op, ASTNode *operand, Loc loc)
{
    ASTUnaryExpr *n = arena_alloc(arena, sizeof(ASTUnaryExpr), sizeof(void *));
    n->base.kind = AST_UNARY_EXPR;
    n->base.loc = loc;
    n->op = op;
    n->operand = operand;
    return &n->base;
}

ASTNode *ast_call_expr(Arena *arena, const char *callee, Vec *args, Loc loc)
{
    ASTCallExpr *n = arena_alloc(arena, sizeof(ASTCallExpr), sizeof(void *));
    n->base.kind = AST_CALL_EXPR;
    n->base.loc = loc;
    n->callee = callee;
    n->args = args;
    return &n->base;
}

ASTNode *ast_ident(Arena *arena, const char *name, Loc loc)
{
    ASTIdent *n = arena_alloc(arena, sizeof(ASTIdent), sizeof(void *));
    n->base.kind = AST_IDENT;
    n->base.loc = loc;
    n->name = name;
    return &n->base;
}

ASTNode *ast_if_stmt(Arena *arena, ASTNode *cond, ASTNode *then_branch, ASTNode *else_branch,
                     Loc loc)
{
    ASTIfStmt *n = arena_alloc(arena, sizeof(ASTIfStmt), sizeof(void *));
    n->base.kind = AST_IF_STMT;
    n->base.loc = loc;
    n->cond = cond;
    n->then_branch = then_branch;
    n->else_branch = else_branch;
    return &n->base;
}

static const char *bin_op_name(BinOpKind op)
{
    switch (op)
    {
        case BIN_ADD:
            return "+";
        case BIN_SUB:
            return "-";
        case BIN_MUL:
            return "*";
        case BIN_DIV:
            return "/";
        case BIN_REM:
            return "%";
        case BIN_ASSIGN:
            return "=";
    }
    return "?";
}

static const char *unary_op_name(UnaryOpKind op)
{
    switch (op)
    {
        case UN_NEG:
            return "-";
    }
    return "?";
}

static void dump_indent(int depth)
{
    for (int i = 0; i < depth; i++)
        printf("  ");
}

static void ast_dump_rec(ASTNode *node, int depth);

void ast_dump(ASTNode *node)
{
    ast_dump_rec(node, 0);
}

static void ast_dump_rec(ASTNode *node, int depth)
{
    if (!node)
    {
        dump_indent(depth);
        printf("(null)\n");
        return;
    }
    dump_indent(depth);
    switch (node->kind)
    {
        case AST_FUNC_DEF:
        {
            ASTFuncDef *n = ast_as(ASTFuncDef, node);
            printf("FUNC_DEF %s -> %s\n", n->name, type_kind_name(n->ret_type->kind));
            ast_dump_rec(n->body, depth + 1);
            break;
        }
        case AST_COMPOUND_STMT:
        {
            ASTCompoundStmt *n = ast_as(ASTCompoundStmt, node);
            printf("COMPOUND_STMT (%zu stmts)\n", vec_size(n->stmts));
            size_t count = vec_size(n->stmts);
            for (size_t i = 0; i < count; i++)
                ast_dump_rec((ASTNode *) vec_get(n->stmts, i), depth + 1);
            break;
        }
        case AST_RETURN_STMT:
        {
            ASTReturnStmt *n = ast_as(ASTReturnStmt, node);
            printf("RETURN\n");
            if (n->expr)
                ast_dump_rec(n->expr, depth + 1);
            break;
        }
        case AST_INT_LITERAL:
        {
            ASTIntLiteral *n = ast_as(ASTIntLiteral, node);
            printf("INT_LITERAL %lld\n", (long long) n->value);
            break;
        }
        case AST_PROGRAM:
        {
            ASTProgram *n = ast_as(ASTProgram, node);
            printf("PROGRAM (%zu decls)\n", vec_size(n->decls));
            size_t count = vec_size(n->decls);
            for (size_t i = 0; i < count; i++)
                ast_dump_rec((ASTNode *) vec_get(n->decls, i), depth + 1);
            break;
        }
        case AST_VAR_DECL:
        {
            ASTVarDecl *n = ast_as(ASTVarDecl, node);
            printf("VAR_DECL %s : %s\n", n->name, type_kind_name(n->type->kind));
            if (n->init)
                ast_dump_rec(n->init, depth + 1);
            break;
        }
        case AST_EXPR_STMT:
        {
            ASTExprStmt *n = ast_as(ASTExprStmt, node);
            printf("EXPR_STMT\n");
            ast_dump_rec(n->expr, depth + 1);
            break;
        }
        case AST_BINARY_EXPR:
        {
            ASTBinaryExpr *n = ast_as(ASTBinaryExpr, node);
            printf("BINARY %s\n", bin_op_name(n->op));
            ast_dump_rec(n->left, depth + 1);
            ast_dump_rec(n->right, depth + 1);
            break;
        }
        case AST_UNARY_EXPR:
        {
            ASTUnaryExpr *n = ast_as(ASTUnaryExpr, node);
            printf("UNARY %s\n", unary_op_name(n->op));
            ast_dump_rec(n->operand, depth + 1);
            break;
        }
        case AST_CALL_EXPR:
        {
            ASTCallExpr *n = ast_as(ASTCallExpr, node);
            printf("CALL %s (%zu args)\n", n->callee, vec_size(n->args));
            size_t count = vec_size(n->args);
            for (size_t i = 0; i < count; i++)
                ast_dump_rec((ASTNode *) vec_get(n->args, i), depth + 1);
            break;
        }
        case AST_IDENT:
        {
            ASTIdent *n = ast_as(ASTIdent, node);
            printf("IDENT %s\n", n->name);
            break;
        }
        case AST_IF_STMT:
        {
            ASTIfStmt *n = ast_as(ASTIfStmt, node);
            printf("IF\n");
            ast_dump_rec(n->cond, depth + 1);
            ast_dump_rec(n->then_branch, depth + 1);
            if (n->else_branch)
                ast_dump_rec(n->else_branch, depth + 1);
            break;
        }
        default:
            fprintf(stderr, "[ast] error: unknown AST kind %s\n", ast_kind_name(node->kind));
            return;
    }
}
