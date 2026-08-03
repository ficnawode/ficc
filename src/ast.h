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
    X(AST_INT_LITERAL)                                                                             \
    X(AST_PROGRAM)                                                                                 \
    X(AST_VAR_DECL)                                                                                \
    X(AST_EXPR_STMT)                                                                               \
    X(AST_BINARY_EXPR)                                                                             \
    X(AST_UNARY_EXPR)                                                                              \
    X(AST_CALL_EXPR)                                                                               \
    X(AST_IDENT)                                                                                   \
    X(AST_IF_STMT)

typedef enum
{
#define ENUM_ENTRY(K) K,
    AST_KINDS(ENUM_ENTRY)
#undef ENUM_ENTRY
} ASTKind;

typedef enum
{
    BIN_ADD,
    BIN_SUB,
    BIN_MUL,
    BIN_DIV,
    BIN_REM,
    BIN_ASSIGN
} BinOpKind;

typedef enum
{
    UN_NEG
} UnaryOpKind;

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

typedef struct ASTProgram ASTProgram;
struct ASTProgram
{
    ASTNode base;
    Vec *decls; /* Vec<ASTNode*> (func defs) */
};

typedef struct ASTVarDecl ASTVarDecl;
struct ASTVarDecl
{
    ASTNode base;
    Type *type;
    const char *name;
    ASTNode *init; /* NULL if no initializer */
};

typedef struct ASTExprStmt ASTExprStmt;
struct ASTExprStmt
{
    ASTNode base;
    ASTNode *expr;
};

typedef struct ASTBinaryExpr ASTBinaryExpr;
struct ASTBinaryExpr
{
    ASTNode base;
    BinOpKind op;
    ASTNode *left;
    ASTNode *right;
};

typedef struct ASTUnaryExpr ASTUnaryExpr;
struct ASTUnaryExpr
{
    ASTNode base;
    UnaryOpKind op;
    ASTNode *operand;
};

typedef struct ASTCallExpr ASTCallExpr;
struct ASTCallExpr
{
    ASTNode base;
    const char *callee;
    Vec *args; /* Vec<ASTNode*> */
};

typedef struct ASTIdent ASTIdent;
struct ASTIdent
{
    ASTNode base;
    const char *name;
};

typedef struct ASTIfStmt ASTIfStmt;
struct ASTIfStmt
{
    ASTNode base;
    ASTNode *cond;
    ASTNode *then_branch;
    ASTNode *else_branch;
};

ASTNode *ast_func_def(Arena *arena, Type *ret_type, const char *name, Vec *params, ASTNode *body,
                      Loc loc);
ASTNode *ast_compound_stmt(Arena *arena, Vec *stmts, Loc loc);
ASTNode *ast_return_stmt(Arena *arena, ASTNode *expr, Loc loc);
ASTNode *ast_int_literal(Arena *arena, i64 value, Loc loc);
ASTNode *ast_program(Arena *arena, Vec *decls, Loc loc);
ASTNode *ast_var_decl(Arena *arena, Type *type, const char *name, ASTNode *init, Loc loc);
ASTNode *ast_expr_stmt(Arena *arena, ASTNode *expr, Loc loc);
ASTNode *ast_binary_expr(Arena *arena, BinOpKind op, ASTNode *left, ASTNode *right, Loc loc);
ASTNode *ast_unary_expr(Arena *arena, UnaryOpKind op, ASTNode *operand, Loc loc);
ASTNode *ast_call_expr(Arena *arena, const char *callee, Vec *args, Loc loc);
ASTNode *ast_ident(Arena *arena, const char *name, Loc loc);
ASTNode *ast_if_stmt(Arena *arena, ASTNode *cond, ASTNode *then_branch, ASTNode *else_branch,
                     Loc loc);

void ast_dump(ASTNode *node);

const char *ast_kind_name(ASTKind kind);

#endif
