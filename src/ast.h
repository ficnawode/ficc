#ifndef FICC_AST_H
#define FICC_AST_H

#include "lexer.h"
#include "type.h"
#include "util/types.h"
#include "util/vec.h"

/* X-macro for AST node kinds. Append only. */
#define AST_KINDS(X)                                                                               \
    X(AST_FUNC_DEF)                                                                                \
    X(AST_COMPOUND_STMT)                                                                           \
    X(AST_RETURN_STMT)                                                                             \
    X(AST_INT_LITERAL)                                                                             \
    X(AST_FLOAT_LITERAL)                                                                           \
    X(AST_PROGRAM)                                                                                 \
    X(AST_VAR_DECL)                                                                                \
    X(AST_EXPR_STMT)                                                                               \
    X(AST_BINARY_EXPR)                                                                             \
    X(AST_INCDEC_EXPR)                                                                             \
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
    X(AST_SWITCH_STMT)                                                                             \
    X(AST_CASE_STMT)                                                                               \
    X(AST_DEFAULT_STMT)                                                                            \
    X(AST_TERNARY_EXPR)                                                                            \
    X(AST_SUBSCRIPT_EXPR)                                                                          \
    X(AST_SIZEOF_EXPR)                                                                             \
    X(AST_SIZEOF_TYPE)                                                                             \
    X(AST_ALIGNOF_EXPR)                                                                            \
    X(AST_ALIGNOF_TYPE)                                                                            \
    X(AST_STRING_LITERAL)                                                                          \
    X(AST_STRUCT_DECL)                                                                             \
    X(AST_ENUM_DECL)                                                                               \
    X(AST_MEMBER_ACCESS)                                                                           \
    X(AST_CAST_EXPR)                                                                               \
    X(AST_TYPEDEF_DECL)                                                                            \
    X(AST_INIT_LIST)                                                                               \
    X(AST_COMPOUND_LITERAL)                                                                        \
    X(AST_DECL_LIST)                                                                               \
    X(AST_STATIC_ASSERT)                                                                           \
    X(AST_VA_ARG_EXPR)                                                                             \
    X(AST_FUNC_DECL)                                                                               \
    X(AST_GENERIC_SELECTION)

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
    BIN_LOG_OR,
    BIN_COMMA,
    BIN_ADD_ASSIGN,
    BIN_SUB_ASSIGN,
    BIN_MUL_ASSIGN,
    BIN_DIV_ASSIGN,
    BIN_REM_ASSIGN,
    BIN_SHL_ASSIGN,
    BIN_SHR_ASSIGN,
    BIN_AND_ASSIGN,
    BIN_OR_ASSIGN,
    BIN_XOR_ASSIGN,
} BinOpKind;

typedef enum
{
    UN_NEG,
    UN_LOG_NOT,
    UN_BIT_NOT,
    UN_DEREF,
    UN_ADDR,
} UnaryOpKind;

typedef enum
{
    SC_NONE,
    SC_STATIC,
    SC_EXTERN,
} StorageClass;

typedef struct ASTNode ASTNode;
struct ASTNode
{
    ASTKind kind;
    Loc loc;
    Type *expr_type; /* computed by semantic pass; non-NULL for expression nodes */
};

#define ast_as(T, node) ((T *) (node))

/* Function declaration specifiers (C11 §6.7.4); one field per new specifier. */
typedef struct
{
    StorageClass storage;
    bool is_inline;
} FuncSpecs;

/* Signature shared by a function definition (`{...}`) and a prototype (`;`). */
typedef struct FuncSig FuncSig;
struct FuncSig
{
    Type *ret_type;
    const char *name;
    Vec *params;      /* Vec<ASTNode*> (parameter declarations; empty for void) */
    FuncSpecs spec;   /* storage class + function specifiers */
    bool is_variadic; /* trailing unnamed args (C11 §6.7.6.3p8) */
    Type *func_type;  /* interned function type (filled by semantic; NULL at parse time) */
};

typedef struct ASTFuncDef ASTFuncDef;
struct ASTFuncDef
{
    ASTNode base;
    FuncSig sig;
    ASTNode *body;
};

/* A function prototype / forward declaration: a signature without a body. */
typedef struct ASTFuncDecl ASTFuncDecl;
struct ASTFuncDecl
{
    ASTNode base;
    FuncSig sig;
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

/* IEEE bits or host long double; only the arm `kind` selects is read. */
typedef union
{
    u64 bits;
    long double ld;
} ASTFloatValue;

typedef struct ASTFloatLiteral ASTFloatLiteral;
struct ASTFloatLiteral
{
    ASTNode base;
    ASTFloatValue value;
    FloatKind kind;
};

typedef struct ASTProgram ASTProgram;
struct ASTProgram
{
    ASTNode base;
    Vec *decls; /* Vec<ASTNode*> — top-level declarations */
};

typedef struct ASTVarDecl ASTVarDecl;
struct ASTVarDecl
{
    ASTNode base;
    Type *type;
    const char *name;
    ASTNode *init; /* NULL if none; an AST_INIT_LIST or AST_STRING_LITERAL for a char array */
    StorageClass storage;
    i64 const_init;        /* folded file-scope constant initializer */
    bool has_const_init;   /* true when const_init is valid */
    bool is_block_scope;   /* declared inside a function body (vs file scope) */
    struct InitPlan *plan; /* aggregate/string flattening plan (filled by semantic) or NULL */
    u32 alignas;           /* requested _Alignas alignment, 0 = natural */
    u32 bit_width;         /* bit-field width in bits; 0 = ordinary member (§6.7.2.1) */
};

/* An init-declarator list `int a = 1, b = 2;` sharing one declaration-specifier sequence (C11
 * §6.7.6). */
typedef struct ASTDeclList ASTDeclList;
struct ASTDeclList
{
    ASTNode base;
    Vec *decls; /* Vec<ASTVarDecl*> */
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

/* Prefix (`++x`) or postfix (`x++`) increment/decrement (C11 §6.5.2.4); its operand is a modifiable
 * lvalue. */
typedef struct ASTIncDecExpr ASTIncDecExpr;
struct ASTIncDecExpr
{
    ASTNode base;
    ASTNode *operand;
    bool is_inc;     /* true: ++ ; false: -- */
    bool is_postfix; /* true: x++;  false: ++x */
};

typedef struct ASTCallExpr ASTCallExpr;
struct ASTCallExpr
{
    ASTNode base;
    const char *callee;   /* named callee / builtin; NULL when callee_expr is used */
    Vec *args;            /* Vec<ASTNode*> */
    ASTNode *callee_expr; /* indirect callee (function designator / pointer value); NULL for named
                             calls */
};

typedef struct ASTIdent ASTIdent;
struct ASTIdent
{
    ASTNode base;
    const char *name;
    ASTVarDecl *decl; /* resolved declaration (filled by semantic); NULL when is_func */
    bool is_func;     /* true when `name` is a function designator; decl stays NULL */
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

typedef struct ASTSwitchStmt ASTSwitchStmt;
struct ASTSwitchStmt
{
    ASTNode base;
    ASTNode *cond;
    ASTNode *body;
};

typedef struct ASTCaseStmt ASTCaseStmt;
struct ASTCaseStmt
{
    ASTNode base;
    ASTNode *expr; /* constant expression (folded into `value` by semantic) */
    i64 value;
    bool value_known; /* parser failed to fold; semantic must resolve `value` */
    Vec *stmts;       /* Vec<ASTNode*>: statements under this label */
};

typedef struct ASTDefaultStmt ASTDefaultStmt;
struct ASTDefaultStmt
{
    ASTNode base;
    Vec *stmts; /* Vec<ASTNode*>: statements from this label up to the next label */
};

typedef struct ASTTernaryExpr ASTTernaryExpr;
struct ASTTernaryExpr
{
    ASTNode base;
    ASTNode *cond;
    ASTNode *then_expr;
    ASTNode *else_expr;
};

typedef struct ASTSubscriptExpr ASTSubscriptExpr;
struct ASTSubscriptExpr
{
    ASTNode base;
    ASTNode *array;
    ASTNode *index;
};

typedef struct ASTSizeofExpr ASTSizeofExpr;
struct ASTSizeofExpr
{
    ASTNode base;
    ASTNode *operand;
    u64 size_value;
};

typedef struct ASTSizeofType ASTSizeofType;
struct ASTSizeofType
{
    ASTNode base;
    Type *type;
    u64 size_value;
};

typedef struct ASTAlignofExpr ASTAlignofExpr;
struct ASTAlignofExpr
{
    ASTNode base;
    ASTNode *operand;
    u64 align_value;
};

typedef struct ASTAlignofType ASTAlignofType;
struct ASTAlignofType
{
    ASTNode base;
    Type *type;
    u64 align_value;
};

typedef struct ASTStaticAssert ASTStaticAssert;
struct ASTStaticAssert
{
    ASTNode base;
    ASTNode *expr;
    const char *msg;
};

typedef struct ASTStringLiteral ASTStringLiteral;
struct ASTStringLiteral
{
    ASTNode base;
    const char *data;
    u64 length;
};

typedef struct ASTStructDecl ASTStructDecl;
struct ASTStructDecl
{
    ASTNode base;
    const char *tag;
    bool is_union;
    Vec *fields; /* Vec<ASTVarDecl*> */
};

typedef struct EnumConstant
{
    const char *name;
    i64 value;
} EnumConstant;

typedef struct ASTEnumDecl ASTEnumDecl;
struct ASTEnumDecl
{
    ASTNode base;
    const char *tag; /* NULL for anonymous enums */
    Vec *constants;  /* Vec<EnumConstant*> */
};

typedef struct ASTMemberAccess ASTMemberAccess;
struct ASTMemberAccess
{
    ASTNode base;
    ASTNode *object;
    const char *member;
    bool is_arrow;
    u32 field_offset; /* filled by semantic */
    Type *field_type; /* filled by semantic */
    bool is_bitfield; /* filled by semantic */
    u32 bit_offset;   /* bit position within the storage unit */
    u32 bit_width;    /* declared width in bits */
};

/* Cast `(type) expr` (C11 §5.5.4): never an lvalue; a qualified target equals the unqualified type
 * (§6.5.4p4). */
typedef struct ASTCastExpr ASTCastExpr;
struct ASTCastExpr
{
    ASTNode base;
    Type *target_type;
    ASTNode *operand;
};

/* `__builtin_va_arg(ap, type)`: its second argument is a type-name, so it is a special form, not a
 * call. */
typedef struct ASTVaArgExpr ASTVaArgExpr;
struct ASTVaArgExpr
{
    ASTNode base;
    ASTNode *ap;
    Type *type;
};

/* A typedef declaration `typedef <type> <name>;` (C11 §6.7.7): the name is an ordinary identifier
 * (§6.2.3). */
typedef struct ASTTypedefDecl ASTTypedefDecl;
struct ASTTypedefDecl
{
    ASTNode base;
    const char *name;
    Type *type;
};

/* Initializer designators (C11 §6.7.9p1): `.field` members / `[idx]` elements, chained
 * outermost-first. */
typedef enum
{
    ND_FIELD, /* .field */
    ND_INDEX, /* [idx] */
} DesignatorKind;

typedef struct Designator Designator;
struct Designator
{
    DesignatorKind kind;
    const char *field; /* ND_FIELD */
    i64 index;         /* ND_INDEX */
    Designator *next;
};

/* One element `[designators] value` of a brace-enclosed initializer list. */
typedef struct InitElem InitElem;
struct InitElem
{
    Designator *design; /* NULL when the element has no designators */
    ASTNode *value;     /* expression, string literal, or nested AST_INIT_LIST */
    Loc loc;
};

/* One flattened write from semantic's initializer planner (scalar, or a char-array string fill). */
typedef struct InitWrite InitWrite;
struct InitWrite
{
    u32 offset;     /* byte offset of the subobject in the initialized object */
    Type *type;     /* unqualified target type (scalar, or char array for a fill) */
    ASTNode *value; /* expression (scalar) or ASTStringLiteral (string fill) */
    bool is_string_fill;
    bool is_bitfield; /* the subobject is a bit-field member (§6.7.2.1) */
    u32 bit_offset;   /* bit position within the storage unit */
    u32 bit_width;    /* field width in bits */
};

/* A brace-enclosed initializer list `{ ... }` (C11 §6.7.9) with semantic's flattened lowering plan.
 */
typedef struct ASTInitList ASTInitList;
struct ASTInitList
{
    ASTNode base;
    Vec *elems; /* Vec<InitElem*> */
    struct InitPlan *plan;
};

typedef struct InitPlan InitPlan;
struct InitPlan
{
    Vec *writes;      /* Vec<InitWrite*> sorted by offset */
    u64 total_size;   /* byte size of the object being initialized */
    bool grow_array;  /* outermost array is `[]` — resized from its initializer */
    u64 inferred_len; /* largest element index+1 reached while grow_array */
};

/* Compound literal `(type){ ... }` (C11 §6.5.2.5): an lvalue denoting an anonymous object. */
typedef struct ASTCompoundLiteral ASTCompoundLiteral;
struct ASTCompoundLiteral
{
    ASTNode base;
    Type *type;            /* the type-name target */
    ASTNode *init;         /* always an AST_INIT_LIST */
    struct InitPlan *plan; /* flattening plan (filled by semantic) */
};

/* One generic association `type : expr` (C11 §6.5.15). `type` is NULL for the
   `default` association, whose expression lives in the selection's
   `default_expr`. */
typedef struct GenericAssoc GenericAssoc;
struct GenericAssoc
{
    Type *type;
    ASTNode *expr;
};

/* Generic selection `_Generic(controlling, assoc-list)` (§6.5.15): a
   compile-time choice among association expressions. Semantic resolves the
   compatible association (or `default`) and records it in `selected`; only
   that expression is evaluated and lowered. */
typedef struct ASTGenericSelection ASTGenericSelection;
struct ASTGenericSelection
{
    ASTNode base;
    ASTNode *controlling;
    Vec *assocs;           /* Vec<GenericAssoc*> — non-default associations */
    ASTNode *default_expr; /* NULL when no `default` association */
    ASTNode *selected;     /* filled by semantic: the chosen expression */
};

ASTNode *ast_func_def(Type *ret_type, const char *name, Vec *params, ASTNode *body, FuncSpecs spec,
                      bool is_variadic, Loc loc, Arena *arena);
ASTNode *ast_func_decl(Type *ret_type, const char *name, Vec *params, FuncSpecs spec,
                       bool is_variadic, Loc loc, Arena *arena);
ASTNode *ast_compound_stmt(Vec *stmts, Loc loc, Arena *arena);
ASTNode *ast_return_stmt(ASTNode *expr, Loc loc, Arena *arena);
ASTNode *ast_int_literal(i64 value, bool is_unsigned, IntSuffix length, bool is_hex, Loc loc,
                         Arena *arena);
ASTNode *ast_float_literal(FloatKind kind, ASTFloatValue value, Loc loc, Arena *arena);
ASTNode *ast_program(Vec *decls, Loc loc, Arena *arena);
ASTNode *ast_var_decl(Type *type, const char *name, ASTNode *init, StorageClass storage, Loc loc,
                      Arena *arena);
ASTNode *ast_decl_list(Vec *decls, Loc loc, Arena *arena);
ASTNode *ast_expr_stmt(ASTNode *expr, Loc loc, Arena *arena);
ASTNode *ast_binary_expr(BinOpKind op, ASTNode *left, ASTNode *right, Loc loc, Arena *arena);
ASTNode *ast_unary_expr(UnaryOpKind op, ASTNode *operand, Loc loc, Arena *arena);
ASTNode *ast_incdec_expr(ASTNode *operand, bool is_inc, bool is_postfix, Loc loc, Arena *arena);
ASTNode *ast_call_expr(const char *callee, Vec *args, Loc loc, Arena *arena);
ASTNode *ast_indirect_call(ASTNode *callee_expr, Vec *args, Loc loc, Arena *arena);
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
ASTNode *ast_switch_stmt(ASTNode *cond, ASTNode *body, Loc loc, Arena *arena);
ASTNode *ast_case_stmt(ASTNode *expr, i64 value, bool value_known, Vec *stmts, Loc loc,
                       Arena *arena);
ASTNode *ast_default_stmt(Vec *stmts, Loc loc, Arena *arena);
ASTNode *ast_ternary_expr(ASTNode *cond, ASTNode *then_expr, ASTNode *else_expr, Loc loc,
                          Arena *arena);

ASTNode *ast_subscript_expr(ASTNode *array, ASTNode *index, Loc loc, Arena *arena);
ASTNode *ast_sizeof_expr(ASTNode *operand, u64 size_value, Loc loc, Arena *arena);
ASTNode *ast_sizeof_type(Type *type, u64 size_value, Loc loc, Arena *arena);
ASTNode *ast_alignof_expr(ASTNode *operand, u64 align_value, Loc loc, Arena *arena);
ASTNode *ast_alignof_type(Type *type, u64 align_value, Loc loc, Arena *arena);
ASTNode *ast_static_assert(ASTNode *expr, const char *msg, Loc loc, Arena *arena);
ASTNode *ast_string_literal(const char *data, u64 len, Loc loc, Arena *arena);
ASTNode *ast_struct_decl(const char *tag, bool is_union, Vec *fields, Loc loc, Arena *arena);
ASTNode *ast_enum_decl(const char *tag, Vec *constants, Loc loc, Arena *arena);
ASTNode *ast_member_access(ASTNode *object, const char *member, bool is_arrow, Loc loc,
                           Arena *arena);
ASTNode *ast_cast_expr(Type *target_type, ASTNode *operand, Loc loc, Arena *arena);
ASTNode *ast_va_arg_expr(ASTNode *ap, Type *type, Loc loc, Arena *arena);
ASTNode *ast_typedef_decl(Type *type, const char *name, Loc loc, Arena *arena);
ASTNode *ast_init_list(Vec *elems, Loc loc, Arena *arena);
ASTNode *ast_compound_literal(Type *type, ASTNode *init, Loc loc, Arena *arena);
ASTNode *ast_generic_selection(ASTNode *controlling, Vec *assocs, ASTNode *default_expr, Loc loc,
                               Arena *arena);

void ast_dump(ASTNode *node);

const char *ast_kind_name(ASTKind kind);

#endif
