#ifndef FICC_IR_BUILDER_H
#define FICC_IR_BUILDER_H

#include "ast.h"
#include "ir.h"

IrModule *ir_build_module(ASTNode *ast, Arena *arena);

#endif
