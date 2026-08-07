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
    X(AST_IF_STMT)                                                                                 \
    X(AST_WHILE_STMT)                                                                              \
    X(AST_DO_WHILE_STMT)                                                                           \
    X(AST_FOR_STMT)                                                                                \
    X(AST_BREAK_STMT)                                                                              \
    X(AST_CONTINUE_STMT)                                                                           \
    X(AST_GOTO_STMT)                                                                               \
    X(AST_LABEL_STMT)                                                                              \
    X(AST_TERNARY_EXPR)

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
    BIN_ASSIGN,
    BIN_EQ,
    BIN_NE,
    BIN_LT,
    BIN_GT,
    BIN_LE,
    BIN_GE,
    BIN_AND,
    BIN_OR,
    BIN_XOR,
    BIN_SHL,
    BIN_SHR,
    BIN_LOG_AND,
    BIN_LOG_OR
} BinOpKind;

typedef enum
{
    UN_NEG,
    UN_LOG_NOT,
    UN_BIT_NOT
} UnaryOpKind;

typedef struct ASTNode ASTNode;
struct ASTNode
{
    ASTKind kind;
    Loc loc;
    Type *expr_type; /* computed by semantic pass; non-NULL for expression nodes */
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
    bool is_unsigned : 1;
    IntSuffix length : 2;
    bool is_hex : 1;
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

typedef struct ASTWhileStmt ASTWhileStmt;
struct ASTWhileStmt
{
    ASTNode base;
    ASTNode *cond;
    ASTNode *body;
};

typedef struct ASTDoWhileStmt ASTDoWhileStmt;
struct ASTDoWhileStmt
{
    ASTNode base;
    ASTNode *cond;
    ASTNode *body;
};

typedef struct ASTForStmt ASTForStmt;
struct ASTForStmt
{
    ASTNode base;
    ASTNode *init; /* may be NULL */
    ASTNode *cond; /* may be NULL */
    ASTNode *post; /* may be NULL */
    ASTNode *body;
};

typedef struct ASTBreakStmt ASTBreakStmt;
struct ASTBreakStmt
{
    ASTNode base;
};

typedef struct ASTContinueStmt ASTContinueStmt;
struct ASTContinueStmt
{
    ASTNode base;
};

typedef struct ASTGotoStmt ASTGotoStmt;
struct ASTGotoStmt
{
    ASTNode base;
    const char *label;
};

typedef struct ASTLabelStmt ASTLabelStmt;
struct ASTLabelStmt
{
    ASTNode base;
    const char *label;
    ASTNode *stmt;
};

typedef struct ASTTernaryExpr ASTTernaryExpr;
struct ASTTernaryExpr
{
    ASTNode base;
    ASTNode *cond;
    ASTNode *then_expr;
    ASTNode *else_expr;
};

ASTNode *ast_func_def(Type *ret_type, const char *name, Vec *params, ASTNode *body, Loc loc,
                      Arena *arena);
ASTNode *ast_compound_stmt(Vec *stmts, Loc loc, Arena *arena);
ASTNode *ast_return_stmt(ASTNode *expr, Loc loc, Arena *arena);
ASTNode *ast_int_literal(i64 value, bool is_unsigned, IntSuffix length, bool is_hex, Loc loc,
                         Arena *arena);
ASTNode *ast_program(Vec *decls, Loc loc, Arena *arena);
ASTNode *ast_var_decl(Type *type, const char *name, ASTNode *init, Loc loc, Arena *arena);
ASTNode *ast_expr_stmt(ASTNode *expr, Loc loc, Arena *arena);
ASTNode *ast_binary_expr(BinOpKind op, ASTNode *left, ASTNode *right, Loc loc, Arena *arena);
ASTNode *ast_unary_expr(UnaryOpKind op, ASTNode *operand, Loc loc, Arena *arena);
ASTNode *ast_call_expr(const char *callee, Vec *args, Loc loc, Arena *arena);
ASTNode *ast_ident(const char *name, Loc loc, Arena *arena);
ASTNode *ast_if_stmt(ASTNode *cond, ASTNode *then_branch, ASTNode *else_branch, Loc loc,
                     Arena *arena);
ASTNode *ast_while_stmt(ASTNode *cond, ASTNode *body, Loc loc, Arena *arena);
ASTNode *ast_do_while_stmt(ASTNode *cond, ASTNode *body, Loc loc, Arena *arena);
ASTNode *ast_for_stmt(ASTNode *init, ASTNode *cond, ASTNode *post, ASTNode *body, Loc loc,
                      Arena *arena);
ASTNode *ast_break_stmt(Loc loc, Arena *arena);
ASTNode *ast_continue_stmt(Loc loc, Arena *arena);
ASTNode *ast_goto_stmt(const char *label, Loc loc, Arena *arena);
ASTNode *ast_label_stmt(const char *label, ASTNode *stmt, Loc loc, Arena *arena);
ASTNode *ast_ternary_expr(ASTNode *cond, ASTNode *then_expr, ASTNode *else_expr, Loc loc,
                          Arena *arena);

void ast_dump(ASTNode *node);

const char *ast_kind_name(ASTKind kind);

#endif
