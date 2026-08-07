#ifndef FICC_IR_BUILDER_H
#define FICC_IR_BUILDER_H

#include "ast.h"
#include "ir.h"

/* Build an IrModule from an annotated AST.
   For Phase 1, the AST must be a single function definition. */
IrModule *ir_build_module(ASTNode *ast, Arena *arena);

#endif
