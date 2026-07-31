#ifndef FICC_AST_H
#define FICC_AST_H

#include "lexer.h"
#include "type.h"
#include "util/assert.h"
#include "util/types.h"
#include "util/vec.h"

/* X-macro for AST node kinds. Append only. */
#define AST_KINDS(X)                                                                               \
    X(AST_FUNC_DEF)                                                                                \
    X(AST_COMPOUND_STMT)                                                                           \
    X(AST_RETURN_STMT)                                                                             \
    X(AST_INT_LITERAL)

typedef enum
{
#define ENUM_ENTRY(K) K,
    AST_KINDS(ENUM_ENTRY)
#undef ENUM_ENTRY
} ASTKind;

typedef struct ASTNode ASTNode;
struct ASTNode
{
    ASTKind kind;
    Loc loc;
};

#define ast_as(T, node) ((T *) (node))

typedef struct ASTFuncDef ASTFuncDef;
struct ASTFuncDef
{
    ASTNode base;
    Type *ret_type;
    const char *name;
    Vec *params; /* Vec<ASTNode*> (parameter declarations; empty for void) */
    ASTNode *body;
};

typedef struct ASTCompoundStmt ASTCompoundStmt;
struct ASTCompoundStmt
{
    ASTNode base;
    Vec *stmts; /* Vec<ASTNode*> */
};

typedef struct ASTReturnStmt ASTReturnStmt;
struct ASTReturnStmt
{
    ASTNode base;
    ASTNode *expr; /* NULL for `return;` */
};

typedef struct ASTIntLiteral ASTIntLiteral;
struct ASTIntLiteral
{
    ASTNode base;
    i64 value;
};

ASTNode *ast_func_def(Arena *arena, Type *ret_type, const char *name, Vec *params, ASTNode *body,
                      Loc loc);
ASTNode *ast_compound_stmt(Arena *arena, Vec *stmts, Loc loc);
ASTNode *ast_return_stmt(Arena *arena, ASTNode *expr, Loc loc);
ASTNode *ast_int_literal(Arena *arena, i64 value, Loc loc);

void ast_dump(ASTNode *node);

const char *ast_kind_name(ASTKind kind);

#endif
