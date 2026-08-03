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

static void *ast_new_node(size_t size, ASTKind kind, Loc loc, Arena *arena)
{
    ASTNode *base = arena_alloc(arena, size, sizeof(void *));
    base->kind = kind;
    base->loc = loc;
    return base;
}

ASTNode *ast_func_def(Type *ret_type, const char *name, Vec *params, ASTNode *body, Loc loc, Arena *arena)
{
    ASTFuncDef *n = ast_new_node(sizeof(ASTFuncDef), AST_FUNC_DEF, loc, arena);
    n->ret_type = ret_type;
    n->name = name;
    n->params = params;
    n->body = body;
    return &n->base;
}

ASTNode *ast_compound_stmt(Vec *stmts, Loc loc, Arena *arena)
{
    ASTCompoundStmt *n = ast_new_node(sizeof(ASTCompoundStmt), AST_COMPOUND_STMT, loc, arena);
    n->stmts = stmts;
    return &n->base;
}

ASTNode *ast_return_stmt(ASTNode *expr, Loc loc, Arena *arena)
{
    ASTReturnStmt *n = ast_new_node(sizeof(ASTReturnStmt), AST_RETURN_STMT, loc, arena);
    n->expr = expr;
    return &n->base;
}

ASTNode *ast_int_literal(i64 value, Loc loc, Arena *arena)
{
    ASTIntLiteral *n = ast_new_node(sizeof(ASTIntLiteral), AST_INT_LITERAL, loc, arena);
    n->value = value;
    return &n->base;
}

ASTNode *ast_program(Vec *decls, Loc loc, Arena *arena)
{
    ASTProgram *n = ast_new_node(sizeof(ASTProgram), AST_PROGRAM, loc, arena);
    n->decls = decls;
    return &n->base;
}

ASTNode *ast_var_decl(Type *type, const char *name, ASTNode *init, Loc loc, Arena *arena)
{
    ASTVarDecl *n = ast_new_node(sizeof(ASTVarDecl), AST_VAR_DECL, loc, arena);
    n->type = type;
    n->name = name;
    n->init = init;
    return &n->base;
}

ASTNode *ast_expr_stmt(ASTNode *expr, Loc loc, Arena *arena)
{
    ASTExprStmt *n = ast_new_node(sizeof(ASTExprStmt), AST_EXPR_STMT, loc, arena);
    n->expr = expr;
    return &n->base;
}

ASTNode *ast_binary_expr(BinOpKind op, ASTNode *left, ASTNode *right, Loc loc, Arena *arena)
{
    ASTBinaryExpr *n = ast_new_node(sizeof(ASTBinaryExpr), AST_BINARY_EXPR, loc, arena);
    n->op = op;
    n->left = left;
    n->right = right;
    return &n->base;
}

ASTNode *ast_unary_expr(UnaryOpKind op, ASTNode *operand, Loc loc, Arena *arena)
{
    ASTUnaryExpr *n = ast_new_node(sizeof(ASTUnaryExpr), AST_UNARY_EXPR, loc, arena);
    n->op = op;
    n->operand = operand;
    return &n->base;
}

ASTNode *ast_call_expr(const char *callee, Vec *args, Loc loc, Arena *arena)
{
    ASTCallExpr *n = ast_new_node(sizeof(ASTCallExpr), AST_CALL_EXPR, loc, arena);
    n->callee = callee;
    n->args = args;
    return &n->base;
}

ASTNode *ast_ident(const char *name, Loc loc, Arena *arena)
{
    ASTIdent *n = ast_new_node(sizeof(ASTIdent), AST_IDENT, loc, arena);
    n->name = name;
    return &n->base;
}

ASTNode *ast_if_stmt(ASTNode *cond, ASTNode *then_branch, ASTNode *else_branch, Loc loc, Arena *arena)
{
    ASTIfStmt *n = ast_new_node(sizeof(ASTIfStmt), AST_IF_STMT, loc, arena);
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
    {
        printf("  ");
    }
}

static void ast_dump_rec(ASTNode *node, int depth);

static void dump_node_list(Vec *nodes, int depth)
{
    size_t count = vec_size(nodes);
    for (size_t i = 0; i < count; i++)
    {
        ast_dump_rec((ASTNode *) vec_get(nodes, i), depth + 1);
    }
}

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
            ASTFuncDef *func_def = ast_as(ASTFuncDef, node);
            printf("FUNC_DEF %s -> %s\n", func_def->name,
                   type_kind_name(func_def->ret_type->kind));
            ast_dump_rec(func_def->body, depth + 1);
            break;
        }
        case AST_COMPOUND_STMT:
        {
            ASTCompoundStmt *compound_stmt = ast_as(ASTCompoundStmt, node);
            printf("COMPOUND_STMT (%zu stmts)\n", vec_size(compound_stmt->stmts));
            dump_node_list(compound_stmt->stmts, depth);
            break;
        }
        case AST_RETURN_STMT:
        {
            ASTReturnStmt *return_stmt = ast_as(ASTReturnStmt, node);
            printf("RETURN\n");
            if (return_stmt->expr)
            {
                ast_dump_rec(return_stmt->expr, depth + 1);
            }
            break;
        }
        case AST_INT_LITERAL:
        {
            ASTIntLiteral *int_literal = ast_as(ASTIntLiteral, node);
            printf("INT_LITERAL %lld\n", (long long) int_literal->value);
            break;
        }
        case AST_PROGRAM:
        {
            ASTProgram *program = ast_as(ASTProgram, node);
            printf("PROGRAM (%zu decls)\n", vec_size(program->decls));
            dump_node_list(program->decls, depth);
            break;
        }
        case AST_VAR_DECL:
        {
            ASTVarDecl *var_decl = ast_as(ASTVarDecl, node);
            printf("VAR_DECL %s : %s\n", var_decl->name, type_kind_name(var_decl->type->kind));
            if (var_decl->init)
            {
                ast_dump_rec(var_decl->init, depth + 1);
            }
            break;
        }
        case AST_EXPR_STMT:
        {
            ASTExprStmt *expr_stmt = ast_as(ASTExprStmt, node);
            printf("EXPR_STMT\n");
            ast_dump_rec(expr_stmt->expr, depth + 1);
            break;
        }
        case AST_BINARY_EXPR:
        {
            ASTBinaryExpr *binary_expr = ast_as(ASTBinaryExpr, node);
            printf("BINARY %s\n", bin_op_name(binary_expr->op));
            ast_dump_rec(binary_expr->left, depth + 1);
            ast_dump_rec(binary_expr->right, depth + 1);
            break;
        }
        case AST_UNARY_EXPR:
        {
            ASTUnaryExpr *unary_expr = ast_as(ASTUnaryExpr, node);
            printf("UNARY %s\n", unary_op_name(unary_expr->op));
            ast_dump_rec(unary_expr->operand, depth + 1);
            break;
        }
        case AST_CALL_EXPR:
        {
            ASTCallExpr *call_expr = ast_as(ASTCallExpr, node);
            printf("CALL %s (%zu args)\n", call_expr->callee, vec_size(call_expr->args));
            dump_node_list(call_expr->args, depth);
            break;
        }
        case AST_IDENT:
        {
            ASTIdent *ident = ast_as(ASTIdent, node);
            printf("IDENT %s\n", ident->name);
            break;
        }
        case AST_IF_STMT:
        {
            ASTIfStmt *if_stmt = ast_as(ASTIfStmt, node);
            printf("IF\n");
            ast_dump_rec(if_stmt->cond, depth + 1);
            ast_dump_rec(if_stmt->then_branch, depth + 1);
            if (if_stmt->else_branch)
            {
                ast_dump_rec(if_stmt->else_branch, depth + 1);
            }
            break;
        }
        default:
            fprintf(stderr, "[ast] error: unknown AST kind %s\n", ast_kind_name(node->kind));
            return;
    }
}
