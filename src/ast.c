#include "ast.h"
#include <stdio.h>

const char *ast_kind_name(ASTKind kind)
{
    switch (kind) {
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
    if (!node) {
        dump_indent(depth);
        printf("(null)\n");
        return;
    }
    dump_indent(depth);
    switch (node->kind) {
    case AST_FUNC_DEF: {
        ASTFuncDef *n = ast_as(ASTFuncDef, node);
        printf("FUNC_DEF %s -> %s\n", n->name, type_kind_name(n->ret_type->kind));
        ast_dump_rec(n->body, depth + 1);
        break;
    }
    case AST_COMPOUND_STMT: {
        ASTCompoundStmt *n = ast_as(ASTCompoundStmt, node);
        printf("COMPOUND_STMT (%zu stmts)\n", vec_size(n->stmts));
        size_t count = vec_size(n->stmts);
        for (size_t i = 0; i < count; i++)
            ast_dump_rec((ASTNode *)vec_get(n->stmts, i), depth + 1);
        break;
    }
    case AST_RETURN_STMT: {
        ASTReturnStmt *n = ast_as(ASTReturnStmt, node);
        printf("RETURN\n");
        if (n->expr)
            ast_dump_rec(n->expr, depth + 1);
        break;
    }
    case AST_INT_LITERAL: {
        ASTIntLiteral *n = ast_as(ASTIntLiteral, node);
        printf("INT_LITERAL %lld\n", (long long)n->value);
        break;
    }
    default:
        printf("UNKNOWN_AST(%d)\n", node->kind);
        break;
    }
}
