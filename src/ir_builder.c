#include "ir_builder.h"
#include <stdarg.h>
#include <stdio.h>

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

static Operand build_expr(ASTNode *node, Function *f, Block *bb, Module *m, Arena *arena)
{
    (void)f;
    (void)bb;
    (void)m;
    (void)arena;

    switch (node->kind) {
    case AST_INT_LITERAL: {
        ASTIntLiteral *lit = (ASTIntLiteral *)node;
        return ir_operand_imm(lit->value);
    }
    default:
        ir_error(node, "unsupported expression kind %s", ast_kind_name(node->kind));
        return ir_operand_imm(0);
    }
}

static void build_stmt(ASTNode *node, Function *f, Block *bb, Module *m, Arena *arena)
{
    switch (node->kind) {
    case AST_RETURN_STMT: {
        ASTReturnStmt *ret = (ASTReturnStmt *)node;
        if (ret->expr) {
            Operand val = build_expr(ret->expr, f, bb, m, arena);
            ir_emit_ret(bb, arena, val);
        } else {
            ir_emit_ret(bb, arena, ir_operand_imm(0));
        }
        return;
    }
    default:
        ir_error(node, "unsupported statement kind %s", ast_kind_name(node->kind));
        return;
    }
}

Module *ir_build_module(ASTNode *ast, Arena *arena)
{
    if (ast->kind != AST_FUNC_DEF) {
        ir_error(ast, "expected function definition at top level");
        return NULL;
    }
    ASTFuncDef *func_ast = (ASTFuncDef *)ast;

    Module *mod = ir_module_new(arena);
    Function *func = ir_module_add_func(mod, arena, func_ast->name, func_ast->ret_type);
    Block *entry = ir_func_add_block(func, arena, "entry");

    if (func_ast->body->kind != AST_COMPOUND_STMT) {
        ir_error(func_ast->body, "expected compound statement as function body");
        return NULL;
    }
    ASTCompoundStmt *body = (ASTCompoundStmt *)func_ast->body;
    size_t nstmts = vec_size(body->stmts);
    for (size_t i = 0; i < nstmts; i++) {
        ASTNode *stmt = (ASTNode *)vec_get(body->stmts, i);
        build_stmt(stmt, func, entry, mod, arena);
    }

    /* Ensure block ends with a terminator */
    if (vec_size(entry->instrs) == 0) {
        ir_emit_unreachable(entry, arena);
    } else {
        Instr *last = (Instr *)vec_get(entry->instrs, vec_size(entry->instrs) - 1);
        if (last->opcode != OP_RET && last->opcode != OP_UNREACHABLE &&
            last->opcode != OP_BR && last->opcode != OP_BRCOND) {
            ir_emit_unreachable(entry, arena);
        }
    }

    return mod;
}
